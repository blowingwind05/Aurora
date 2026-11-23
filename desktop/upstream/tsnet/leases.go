// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"encoding/json"
	"errors"
)

func (n *embeddedNode) closeLease(raw json.RawMessage) (any, error) {
	var args struct {
		Lease   string `json:"lease"`
		LeaseID string `json:"leaseId"`
	}
	if err := decodeParams(raw, &args); err != nil {
		return nil, err
	}
	if (args.Lease == "") == (args.LeaseID == "") {
		return nil, errors.New("close requires exactly one lease identifier")
	}
	if args.Lease != "" {
		if entry, ok := n.leases[args.Lease]; ok {
			entry.relay.Close()
			delete(n.leases, args.Lease)
		}
	} else {
		n.closeAuxLease(args.LeaseID)
	}
	return struct{}{}, nil
}
