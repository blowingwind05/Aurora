// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"sync"
)

const maxRequestBytes = 256 * 1024
const maxPresenceRequests = 8

type rpcRequest struct {
	ID     json.RawMessage `json:"id"`
	Method string          `json:"method"`
	Params json.RawMessage `json:"params"`
}

type rpcResponse struct {
	ID     json.RawMessage `json:"id"`
	OK     bool            `json:"ok"`
	Result any             `json:"result"`
	Error  *string         `json:"error"`
}

func serveRPC(ctx context.Context, input io.Reader, output io.Writer,
	handle func(context.Context, string, json.RawMessage) (any, error),
) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	scanner := bufio.NewScanner(input)
	scanner.Buffer(make([]byte, 4096), maxRequestBytes)
	encoder := json.NewEncoder(output)
	var writeMu sync.Mutex
	var writeErr error
	writeReply := func(request rpcRequest, result any, err error) {
		response := rpcResponse{ID: request.ID, OK: err == nil, Result: result}
		if !validRequestID(response.ID) {
			response.ID = nil
		}
		if err != nil {
			message := err.Error()
			response.Error = &message
			response.Result = nil
		}
		writeMu.Lock()
		defer writeMu.Unlock()
		if writeErr == nil {
			if err := encoder.Encode(response); err != nil {
				writeErr = fmt.Errorf("write RPC reply: %w", err)
				cancel()
				if closer, ok := input.(io.Closer); ok {
					_ = closer.Close()
				}
			}
		}
	}

	var workers sync.WaitGroup
	slots := make(chan struct{}, maxPresenceRequests)
	for ctx.Err() == nil && scanner.Scan() {
		var request rpcRequest
		err := decodeJSON(scanner.Bytes(), &request)
		if err == nil && (!validRequestID(request.ID) || request.Method == "") {
			err = errors.New("id and method are required")
		}
		if err != nil {
			writeReply(request, nil, err)
			continue
		}
		if request.Method == "presence" {
			select {
			case slots <- struct{}{}:
				workers.Go(func() {
					result, err := handle(ctx, request.Method, request.Params)
					writeReply(request, result, err)
					<-slots
				})
			default:
				writeReply(request, nil, errors.New("too many simultaneous presence requests"))
			}
			continue
		}
		result, err := handle(ctx, request.Method, request.Params)
		writeReply(request, result, err)
	}
	previousErr := ctx.Err()
	cancel()
	workers.Wait()
	if writeErr != nil {
		return writeErr
	}
	if err := scanner.Err(); err != nil && previousErr == nil {
		return fmt.Errorf("read RPC request: %w", err)
	}
	return nil
}

func decodeJSON(data []byte, target any) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return fmt.Errorf("invalid JSON request: %w", err)
	}
	if err := decoder.Decode(new(json.RawMessage)); err != io.EOF {
		return errors.New("expected exactly one JSON value")
	}
	return nil
}

func decodeParams(data json.RawMessage, target any) error {
	value := bytes.TrimSpace(data)
	if len(value) == 0 || bytes.Equal(value, []byte("null")) {
		value = []byte("{}")
	}
	if value[0] != '{' {
		return errors.New("params must be an object")
	}
	return decodeJSON(value, target)
}

func validRequestID(data json.RawMessage) bool {
	value := bytes.TrimSpace(data)
	if len(value) == 0 {
		return false
	}
	if value[0] == '"' {
		return true
	}
	// Keep the raw number so large request IDs are echoed without rounding.
	for i, digit := range value {
		if (digit < '0' || digit > '9') && (i != 0 || digit != '-') {
			return false
		}
	}
	return true
}
