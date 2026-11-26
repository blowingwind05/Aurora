const preview = document.querySelector('.preview');
const session = preview.querySelector('.session');
const bar = session.querySelector('.session-bar');
const status = session.querySelector('.status');
const statusAnnouncement = session.querySelector('[data-state-text]');
const connectionDetails = session.querySelector('.connection-panel');
const display = session.querySelector('.compact-display');
const displayDetails = session.querySelector('.display-panel');
const displayTabs = session.querySelector('.display-tabs');
const displayTabTemplate = session.querySelector('#display-tab-template');
const displayCloseTemplate = session.querySelector('#display-close-template');
const displayOptions = displayDetails.querySelector('.display-options');
const addDisplay = session.querySelector('.display-add');
const displayProgress = session.querySelector('[data-display-progress]');
const displayError = session.querySelector('.display-error');
const frame = session.querySelector('.frame');
const fullscreen = session.querySelector('.fullscreen');
const controls = session.querySelector('.controls');
const disconnect = session.querySelector('.disconnect');
const cancelConnection = session.querySelector('.cancel-connection');
const menu = session.querySelector('.control-panel');
const controlViews = menu.querySelectorAll('[data-control-view]');
const bitrate = menu.querySelector('#control-bitrate');
const keyHint = menu.querySelector('.key-hint');
const desktopMode = menu.querySelector('[value="desktop"]');
const reopen = preview.querySelector('.preview-closed');
const closedReason = reopen.querySelector('[data-preview-closed-reason]');
const qualitySelect = preview.querySelector('[data-preview-quality]');
const phases = {
  connecting: { status: '连接中' },
  connected: { status: '已连接' },
  failed: { status: '连接失败' }
};
const qualities = {
  good: { label: '已连接', latency: '12 ms', fps: '59.9 fps', bitrate: '18.6 Mbps', loss: '0.0%' },
  poor: { label: '网络较差', latency: '108 ms', fps: '56.1 fps', bitrate: '12.4 Mbps', loss: '3.8%' },
  bad: { label: '网络很差', latency: '310 ms', fps: '29.4 fps', bitrate: '4.7 Mbps', loss: '12.4%' }
};
const primaryDisplay = {
  id: 'primary', name: '显示器 1', type: '主显示器', resolution: '2560 × 1440', refresh: '60 Hz', scale: '125%',
  poster: 'assets/wallpapers/default/mountain-valley.webp', alt: '主显示器串流示例，晴空山谷壁纸'
};
const virtualPoster = new Image();
virtualPoster.src = 'assets/wallpapers/default/baikal-piano.webp';
let displays = [primaryDisplay];
let activeDisplay = primaryDisplay;
let nextVirtualDisplay = 1;
let displayCreation = null;
let openPopover = null;
let controlView = 'root';

function nameButton(button, label, glyph) {
  button.setAttribute('aria-label', label);
  button.title = label;
  const text = button.querySelector('span');
  if (text) text.textContent = label;
  if (glyph) button.querySelector('use').setAttribute('href', '#glyph-' + glyph);
}

function closePopover(restoreFocus = false) {
  if (!openPopover) return;
  const { trigger, panel } = openPopover;
  openPopover = null;
  panel.hidden = true;
  trigger.setAttribute('aria-expanded', 'false');
  if (restoreFocus) trigger.focus({ preventScroll: true });
}

function togglePopover(trigger, panel, focusTarget = panel) {
  if (openPopover?.trigger === trigger) {
    closePopover(true);
    return;
  }
  closePopover();
  panel.hidden = false;
  trigger.setAttribute('aria-expanded', 'true');
  openPopover = { trigger, panel };
  focusTarget.focus({ preventScroll: true });
}

function showControlView(view) {
  for (const pane of controlViews) pane.hidden = pane.dataset.controlView !== view;
  controlView = view;
  menu.scrollTop = 0;
}

function backToControls() {
  const current = controlView;
  showControlView('root');
  menu.querySelector('[data-control-page="' + current + '"]').focus({ preventScroll: true });
}

function displayCloseButton(item, connected) {
  const close = displayCloseTemplate.content.firstElementChild.cloneNode(true);
  close.dataset.closeDisplay = item.id;
  close.title = '关闭' + item.name;
  close.setAttribute('aria-label', close.title);
  close.disabled = !connected;
  return close;
}

function renderDisplays() {
  displayTabs.replaceChildren();
  displayTabs.style.setProperty('--display-count', String(displays.length));
  displayOptions.replaceChildren();
  const connected = session.dataset.phase === 'connected';
  for (const item of displays) {
    const selected = item === activeDisplay;
    const tab = displayTabTemplate.content.firstElementChild.cloneNode(true);
    tab.dataset.displayId = item.id;
    tab.querySelector('span').textContent = item.tabName || item.name;
    tab.setAttribute('aria-pressed', String(selected));
    tab.title = item.name + (selected ? '，查看显示器详情' : '，切换显示器');
    tab.setAttribute('aria-label', tab.title);
    tab.disabled = !connected;
    if (selected) {
      tab.setAttribute('aria-controls', displayDetails.id);
      tab.setAttribute('aria-expanded', 'false');
    }
    const tabItem = document.createElement('div');
    tabItem.className = 'display-tab-item';
    tabItem.append(tab);
    if (item !== primaryDisplay) tabItem.append(displayCloseButton(item, connected));
    displayTabs.append(tabItem);

    const option = document.createElement('button');
    option.type = 'button';
    option.dataset.displayId = item.id;
    option.textContent = item.name;
    option.setAttribute('aria-pressed', String(selected));
    const optionRow = document.createElement('div');
    optionRow.className = 'display-option';
    optionRow.append(option);
    if (item !== primaryDisplay) optionRow.append(displayCloseButton(item, connected));
    displayOptions.append(optionRow);
  }
  displayOptions.hidden = displays.length < 2;
  for (const field of displayDetails.querySelectorAll('[data-display-field]')) {
    field.textContent = activeDisplay[field.dataset.displayField];
  }
  display.title = activeDisplay.name + '，查看显示器列表与详情';
  display.setAttribute('aria-label', display.title);
  frame.src = activeDisplay.poster;
  frame.alt = activeDisplay.alt;
}

function keepDisplayTabVisible() {
  const selectedTab = displayTabs.querySelector('[aria-pressed="true"]');
  if (!selectedTab || !displayTabs.getClientRects().length) return;
  const tab = selectedTab.closest('.display-tab-item').getBoundingClientRect();
  const strip = displayTabs.getBoundingClientRect();
  const foot = parseFloat(getComputedStyle(displayTabs).paddingInlineStart);
  const rightOverflow = tab.right + foot - strip.right;
  const leftOverflow = tab.left - foot - strip.left;
  if (rightOverflow > 0) displayTabs.scrollLeft += rightOverflow;
  else if (leftOverflow < 0) displayTabs.scrollLeft += leftOverflow;
}

function selectDisplay(item) {
  closePopover();
  activeDisplay = item;
  renderDisplays();
  if (display.getClientRects().length) {
    display.focus({ preventScroll: true });
  } else {
    keepDisplayTabVisible();
    displayTabs.querySelector('[aria-pressed="true"]').focus({ preventScroll: true });
  }
}

function closeVirtualDisplay(id) {
  if (session.dataset.phase !== 'connected') return;
  const index = displays.findIndex((item) => item.id === id);
  if (index <= 0) return;
  const [closed] = displays.splice(index, 1);
  selectDisplay(closed === activeDisplay ? displays[index - 1] : activeDisplay);
  displayProgress.textContent = '已关闭' + closed.name;
}

function renderDisplayCreation(message = '', failed = false) {
  const pending = displayCreation !== null;
  const connected = session.dataset.phase === 'connected';
  addDisplay.disabled = pending || !connected;
  addDisplay.setAttribute('aria-busy', String(pending));
  addDisplay.title = pending ? '正在创建虚拟显示器…' : '新增虚拟显示器';
  displayProgress.textContent = message;
  displayError.hidden = !failed;
}

function cancelDisplayCreation() {
  clearTimeout(displayCreation);
  displayCreation = null;
  renderDisplayCreation();
}

function renderQuality() {
  const quality = qualitySelect.value;
  const sample = qualities[quality];
  const connected = session.dataset.phase === 'connected';
  const label = connected ? sample.label : phases[session.dataset.phase].status;
  session.dataset.quality = quality;
  status.title = connected ? '连接详情' : label;
  status.setAttribute('aria-label', connected ? label + '，查看连接详情' : label);
  status.querySelector('span').textContent = label;
  statusAnnouncement.textContent = label;
  for (const metric of connectionDetails.querySelectorAll('[data-metric]')) {
    metric.textContent = sample[metric.dataset.metric];
  }
}

function setFullscreen(active) {
  closePopover();
  session.classList.toggle('is-fullscreen', active);
  session.classList.remove('reveal-bar');
  nameButton(fullscreen, active ? '退出全屏' : '全屏', active ? 'unfull' : 'full');
  fullscreen.setAttribute('aria-pressed', String(active));
}

function setMaximized(active) {
  session.classList.toggle('is-maximized', active);
  nameButton(session.querySelector('[data-window="maximize"]'), active ? '还原窗口' : '最大化窗口', active ? 'restore' : 'max');
}

function setPhase(phase) {
  closePopover();
  session.dataset.phase = phase;
  const connected = phase === 'connected';
  display.disabled = status.disabled = controls.disabled = fullscreen.disabled = !connected;
  if (!connected) cancelDisplayCreation();
  else renderDisplayCreation();
  renderDisplays();
  renderQuality();
  if (!connected) setFullscreen(false);
  if (phase === 'connecting') {
    desktopMode.checked = true;
    keyHint.hidden = true;
  }
  disconnect.disabled = !connected;
  cancelConnection.hidden = phase !== 'connecting';
  for (const button of preview.querySelectorAll('[data-preview-phase]')) {
    button.setAttribute('aria-pressed', String(button.dataset.previewPhase === phase));
  }
}

function setPlatform(platform) {
  closePopover();
  session.dataset.platform = platform;
  keyHint.hidden = desktopMode.checked || platform !== 'windows';
  for (const button of preview.querySelectorAll('[data-preview-platform]')) {
    button.setAttribute('aria-pressed', String(button.dataset.previewPlatform === platform));
  }
}

function showWindow() {
  session.hidden = false;
  reopen.hidden = true;
}

function hideWindow(reason = '') {
  setFullscreen(false);
  setMaximized(false);
  cancelDisplayCreation();
  displays = [primaryDisplay];
  activeDisplay = primaryDisplay;
  nextVirtualDisplay = 1;
  renderDisplays();
  session.hidden = true;
  session.dispatchEvent(new Event('preview-session-closed'));
  closedReason.textContent = reason;
  closedReason.hidden = !reason;
  reopen.hidden = false;
  reopen.querySelector('button').focus({ preventScroll: true });
}

controls.addEventListener('click', () => {
  showControlView('root');
  togglePopover(controls, menu, menu.querySelector('[data-control-page]'));
});

status.addEventListener('click', () => togglePopover(status, connectionDetails));
display.addEventListener('click', () => togglePopover(display, displayDetails));
displayTabs.addEventListener('transitionend', (event) => {
  if (event.target === displayTabs) keepDisplayTabVisible();
});
displayTabs.addEventListener('click', (event) => {
  const close = event.target.closest('[data-close-display]');
  if (close) {
    closeVirtualDisplay(close.dataset.closeDisplay);
    return;
  }
  const tab = event.target.closest('[data-display-id]');
  if (!tab) return;
  const item = displays.find((candidate) => candidate.id === tab.dataset.displayId);
  if (item === activeDisplay) togglePopover(tab, displayDetails);
  else selectDisplay(item);
});
displayOptions.addEventListener('click', (event) => {
  const close = event.target.closest('[data-close-display]');
  if (close) {
    closeVirtualDisplay(close.dataset.closeDisplay);
    return;
  }
  const option = event.target.closest('[data-display-id]');
  if (option) selectDisplay(displays.find((item) => item.id === option.dataset.displayId));
});
addDisplay.addEventListener('click', () => {
  if (displayCreation !== null || session.dataset.phase !== 'connected') return;
  closePopover();
  const number = nextVirtualDisplay;
  const newDisplay = {
    id: 'virtual-' + number, name: '虚拟显示器 ' + number, tabName: '虚拟屏 ' + number, type: '虚拟显示器',
    resolution: '1920 × 1080', refresh: '60 Hz', scale: '100%',
    poster: virtualPoster.src, alt: '虚拟显示器串流示例，冰湖钢琴壁纸'
  };
  const request = setTimeout(async () => {
    try {
      await virtualPoster.decode();
    } catch {
      if (displayCreation !== request) return;
      displayCreation = null;
      renderDisplayCreation(displayError.textContent, true);
      return;
    }
    if (displayCreation !== request) return;
    displayCreation = null;
    nextVirtualDisplay += 1;
    displays.push(newDisplay);
    selectDisplay(newDisplay);
    renderDisplayCreation('已切换至' + newDisplay.name);
  }, 650);
  displayCreation = request;
  renderDisplayCreation('正在创建虚拟显示器…');
});
qualitySelect.addEventListener('change', renderQuality);

menu.addEventListener('click', (event) => {
  const next = event.target.closest('[data-control-page]');
  if (next) {
    showControlView(next.dataset.controlPage);
    menu.querySelector('[data-control-view]:not([hidden]) [data-control-back]').focus({ preventScroll: true });
  } else if (event.target.closest('[data-control-back]')) {
    backToControls();
  }
});
menu.addEventListener('change', (event) => {
  if (event.target.name === 'mouse') keyHint.hidden = desktopMode.checked || session.dataset.platform !== 'windows';
});
bitrate.addEventListener('input', () => { menu.querySelector('output[for="control-bitrate"]').value = bitrate.value + ' Mbps'; });

document.addEventListener('pointerdown', (event) => {
  if (!openPopover) return;
  const { trigger, panel } = openPopover;
  if (!panel.contains(event.target) && !trigger.contains(event.target)) closePopover();
});

document.addEventListener('keydown', (event) => {
  if (preview.ownerDocument.querySelector('.privacy-editor[open]')) return;
  if (event.key === 'Escape' && openPopover) {
    if (openPopover.panel === menu && controlView !== 'root') backToControls();
    else closePopover(true);
    event.preventDefault();
  } else if (event.key === 'Escape' && session.classList.contains('is-fullscreen')) {
    setFullscreen(false);
    controls.focus({ preventScroll: true });
    event.preventDefault();
  } else if (event.ctrlKey && event.altKey && event.shiftKey && event.code === 'KeyZ') {
    session.classList.add('reveal-bar');
  }
});

session.addEventListener('pointermove', (event) => {
  if (!session.classList.contains('is-fullscreen')) return;
  if (event.clientY <= 8) session.classList.add('reveal-bar');
  if (event.clientY > 44 && !bar.contains(event.target) && !openPopover) {
    session.classList.remove('reveal-bar');
  }
});

fullscreen.addEventListener('click', () => {
  const active = !session.classList.contains('is-fullscreen');
  setFullscreen(active);
  if (!active) controls.focus({ preventScroll: true });
});
disconnect.addEventListener('click', () => hideWindow());
cancelConnection.addEventListener('click', () => hideWindow());
preview.querySelector('[data-preview-offline]').addEventListener('click', () => hideWindow('书房主机已离线'));
preview.querySelector('.failure .retry').addEventListener('click', () => {
  setPhase('connecting');
  cancelConnection.focus({ preventScroll: true });
});
reopen.querySelector('button').addEventListener('click', showWindow);

for (const button of preview.querySelectorAll('[data-preview-phase]')) {
  button.addEventListener('click', () => {
    showWindow();
    setPhase(button.dataset.previewPhase);
  });
}
for (const button of preview.querySelectorAll('[data-preview-platform]')) {
  button.addEventListener('click', () => setPlatform(button.dataset.previewPlatform));
}
for (const button of session.querySelectorAll('[data-window]')) {
  button.addEventListener('click', () => {
    const action = button.dataset.window;
    if (action === 'maximize') setMaximized(!session.classList.contains('is-maximized'));
    else if (action === 'zoom' && session.dataset.phase === 'connected') setFullscreen(!session.classList.contains('is-fullscreen'));
    else if (action === 'zoom') setMaximized(!session.classList.contains('is-maximized'));
    else hideWindow();
  });
}

setPhase('connected');
