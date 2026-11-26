const app = document.querySelector('.mobile-app');
const panels = [...app.querySelectorAll('[data-view-panel]')];
const tabs = [...app.querySelectorAll('.tab-bar button')];
const sheet = app.querySelector('.mobile-sheet');
const sheetHeader = sheet.querySelector('.sheet-header');
const sheetBody = sheet.querySelector('.sheet-body');
const sheetPanels = [...sheet.querySelectorAll('[data-sheet-panel]')];
const toast = app.querySelector('.toast');
const image = app.querySelector('.stream-stage img');
const stream = app.querySelector('.stream');
const accountList = app.querySelector('[data-account-list]');
const profiles = [
  { name: '林小满', login: 'xiaoman@github', network: 'aurora-demo', address: '100.100.0.18' },
  { name: '周青', login: 'qing@example.com', network: 'lab-demo', address: '100.100.0.24' }
];
const devices = {
  study: { name: '书房主机', lan: '192.168.1.36', tailscale: '100.100.0.36', online: true },
  living: { name: '客厅电脑', tailscale: '100.100.0.56', online: false }
};
const displayPhotos = {
  主显示器: 'mountain-valley',
  '显示器 2': 'baikal-piano'
};
const wallpapers = [
  ['mountain-valley', '晴空山谷'], ['forest-path', '林间小径'],
  ['blossom-grove', '花树下'], ['ginkgo-lakeside', '银杏湖畔'],
  ['baikal-piano', '冰湖钢琴'], ['baikal-accordion', '冰湖手风琴'],
  ['sunflower-field', '向日葵花田'], ['sunset-sky', '逆光云海']
].map(([id, title]) => ({ title, url: 'assets/wallpapers/default/' + id + '.webp' }));
const sheetTitles = {
  feedback: '反馈', control: '控制中心', quality: '画质', input: '输入方式',
  display: '显示器', privacy: '隐私保护', screensaver: '设置隐私屏保', keyboard: '键盘',
  disconnect: '断开远控', 'add-account': '添加 Tailscale 账户', logout: '退出登录'
};

let signedIn = true;
let activeProfile = profiles[0];
let accountSwitchTimer;
let selectedDevice = 'study';
let powerAction;
let toastTimer;
let savedWallpaper = wallpapers[0];
let draftWallpaper = savedWallpaper;
let sheetDrag;
let sheetCloseTimer;

function navigate(view) {
  closeSheet();
  clearTimeout(toastTimer);
  toast.hidden = true;
  app.dataset.view = view;
  panels.forEach(panel => { panel.hidden = panel.dataset.viewPanel !== view; });
  tabs.forEach(tab => {
    if (tab.dataset.nav === view) tab.setAttribute('aria-current', 'page');
    else if ((tab.dataset.nav === 'more' && ['settings', 'about'].includes(view)) ||
      (tab.dataset.nav === 'account' && view === 'accounts')) tab.setAttribute('aria-current', 'true');
    else tab.removeAttribute('aria-current');
  });
  app.querySelector('.mobile-content').scrollTop = 0;
  if (view === 'stream') stream.focus({ preventScroll: true });
  if (view === 'accounts' || view === 'about') {
    app.querySelector('[data-view-panel="' + view + '"] h1').focus();
  }
}

function message(text, duration = 1800) {
  clearTimeout(toastTimer);
  toast.textContent = text;
  toast.hidden = false;
  toastTimer = setTimeout(() => { toast.hidden = true; }, duration);
}

function renderAccount() {
  const hasDemoDevices = activeProfile === profiles[0];
  app.querySelector('[data-device-list]').hidden = !signedIn || !hasDemoDevices;
  app.querySelector('[data-devices-empty]').hidden = !signedIn || hasDemoDevices;
  app.querySelector('[data-signed-out]').hidden = signedIn;
  app.querySelector('[data-account-details]').hidden = !signedIn;
  app.querySelector('[data-account-signin]').hidden = signedIn;
  app.querySelector('.tab-bar .avatar').hidden = !signedIn;
  app.querySelector('[data-signed-out-mark]').hidden = signedIn;
  app.querySelectorAll('[data-account-initial]').forEach(label => { label.textContent = activeProfile.name[0]; });
  for (const key of ['name', 'login', 'network', 'address']) {
    app.querySelector('[data-account-' + key + ']').textContent = activeProfile[key];
  }
  renderAccountList();
}

function renderAccountList() {
  const rows = profiles.map(profile => {
    const row = document.createElement('li');
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'account-option';
    button.innerHTML = '<span class="avatar" aria-hidden="true"></span><span class="account-profile"><strong></strong><small></small></span><i class="icon check" aria-hidden="true" hidden></i>';
    button.querySelector('.avatar').textContent = profile.name[0];
    button.querySelector('strong').textContent = profile.name;
    button.querySelector('small').textContent = profile.login + ' · ' + profile.network;
    if (profile === activeProfile) {
      button.setAttribute('aria-current', 'true');
      button.querySelector('.check').hidden = false;
    } else {
      button.addEventListener('click', () => switchAccount(profile, button));
    }
    row.append(button);
    return row;
  });
  accountList.replaceChildren(...rows);
}

function switchAccount(profile, button) {
  if (accountSwitchTimer) return;
  const marker = button.querySelector('.check');
  marker.className = 'spinner';
  marker.hidden = false;
  accountList.setAttribute('aria-busy', 'true');
  accountSwitchTimer = setTimeout(() => {
    accountSwitchTimer = undefined;
    accountList.removeAttribute('aria-busy');
    if (app.dataset.view === 'accounts') activateProfile(profile, '已切换至');
    else {
      activeProfile = profile;
      renderAccount();
      message('已切换至' + profile.name);
    }
  }, 450);
}

function activateProfile(profile, feedback) {
  activeProfile = profile;
  renderAccount();
  navigate('account');
  app.querySelector('[data-nav="accounts"]').focus();
  message(feedback + profile.name);
}

function showSheet(name) {
  resetSheetDrag();
  if (name === 'screensaver') selectWallpaper(savedWallpaper);
  const device = devices[selectedDevice];
  let title = sheetTitles[name];
  if (name === 'device-more') {
    title = device.name;
    sheet.querySelector('[data-copy="lan"]').hidden = !device.lan;
    sheet.querySelectorAll('[data-power]').forEach(button => { button.disabled = !device.online; });
  }
  if (name === 'device-power') {
    title = '要' + powerAction + '「' + device.name + '」吗？';
    sheet.querySelector('[data-power-note]').textContent = powerAction === '关机'
      ? '关机将断开当前连接。重新开机后才能再次连接，请先保存远端工作。'
      : '重启将断开当前连接，请先保存远端工作。';
    sheet.querySelector('[data-power-confirm]').textContent = '确认' + powerAction;
  }
  sheet.querySelector('h2').textContent = title;
  sheetPanels.forEach(panel => { panel.hidden = panel.dataset.sheetPanel !== name; });
  sheetBody.scrollTop = 0;
  if (!sheet.open) sheet.showModal();
  if (name === 'keyboard') sheet.querySelector('textarea').focus();
  if (name === 'device-power') sheet.querySelector('[data-sheet-panel="device-power"] [data-cancel]').focus();
}

function closeSheet() {
  resetSheetDrag();
  if (sheet.open) sheet.close();
}

function resetSheetDrag() {
  clearTimeout(sheetCloseTimer);
  sheetCloseTimer = undefined;
  sheetDrag = undefined;
  delete sheet.dataset.dragging;
  sheet.style.removeProperty('--sheet-offset');
}

function startSheetDrag(point, id) {
  if (sheetDrag || sheetCloseTimer) return false;
  const rotated = getComputedStyle(sheet).getPropertyValue('--sheet-rotated').trim() === '1';
  sheetDrag = {
    id, rotated, x: point.clientX, y: point.clientY,
    start: performance.now(), distance: 0
  };
  return true;
}

function moveSheetDrag(point, id) {
  if (sheetDrag?.id !== id) return false;
  const dx = point.clientX - sheetDrag.x;
  const dy = point.clientY - sheetDrag.y;
  const distance = sheetDrag.rotated ? -dx : dy;
  const sideways = sheetDrag.rotated ? dy : dx;
  if (!sheetDrag.distance) {
    if (Math.max(Math.abs(distance), Math.abs(sideways)) < 8) return false;
    if (distance <= Math.abs(sideways)) {
      resetSheetDrag();
      return false;
    }
  }
  sheetDrag.distance = Math.max(0, distance);
  sheet.dataset.dragging = '';
  sheet.style.setProperty('--sheet-offset', sheetDrag.distance + 'px');
  return true;
}

function finishSheetDrag(id, cancelled = false) {
  if (sheetDrag?.id !== id) return;
  const { distance, start, rotated } = sheetDrag;
  sheetDrag = undefined;
  delete sheet.dataset.dragging;
  const fastSwipe = distance >= 36 && distance / Math.max(1, performance.now() - start) >= 0.55;
  if (cancelled || (distance < 84 && !fastSwipe)) {
    sheet.style.removeProperty('--sheet-offset');
    return;
  }
  if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
    closeSheet();
    return;
  }
  const bounds = sheet.getBoundingClientRect();
  const remaining = rotated ? bounds.right : window.innerHeight - bounds.top;
  sheet.style.setProperty('--sheet-offset', (distance + remaining + 2) + 'px');
  sheetCloseTimer = setTimeout(closeSheet, 180);
}

sheet.addEventListener('close', () => { if (!sheet.open) resetSheetDrag(); });

sheetHeader.addEventListener('pointerdown', event => {
  if (!event.isPrimary || event.button !== 0 || event.target.closest('button')) return;
  if (startSheetDrag(event, event.pointerId)) sheetHeader.setPointerCapture(event.pointerId);
});
sheetHeader.addEventListener('pointermove', event => moveSheetDrag(event, event.pointerId));
sheetHeader.addEventListener('pointerup', event => finishSheetDrag(event.pointerId));
sheetHeader.addEventListener('lostpointercapture', event => finishSheetDrag(event.pointerId, true));

sheetBody.addEventListener('touchstart', event => {
  if (event.touches.length !== 1 || sheetBody.scrollTop > 0 || event.target.closest('textarea, input')) return;
  const [touch] = event.changedTouches;
  startSheetDrag(touch, touch.identifier);
}, { passive: true });
sheetBody.addEventListener('touchmove', event => {
  const [touch] = event.changedTouches;
  if (event.touches.length !== 1) {
    finishSheetDrag(touch.identifier, true);
    return;
  }
  if (event.cancelable && moveSheetDrag(touch, touch.identifier)) event.preventDefault();
}, { passive: false });
sheetBody.addEventListener('touchend', event => finishSheetDrag(event.changedTouches[0].identifier));
sheetBody.addEventListener('touchcancel', event => finishSheetDrag(event.changedTouches[0].identifier, true));

function setChoice(kind, current) {
  sheet.querySelectorAll('[data-' + kind + ']').forEach(button => {
    button.setAttribute('aria-pressed', String(button.dataset[kind] === current));
  });
  app.querySelectorAll('[data-' + kind + '-value]').forEach(label => { label.textContent = current; });
}

function selectWallpaper(choice) {
  const previous = draftWallpaper;
  draftWallpaper = choice;
  if (previous.custom && previous !== savedWallpaper && previous !== choice) URL.revokeObjectURL(previous.url);
  sheet.querySelector('[data-screensaver-preview]').src = choice.url;
  [...sheet.querySelector('[data-presets]').children].forEach((button, index) => {
    button.setAttribute('aria-pressed', String(wallpapers[index] === choice));
  });
  sheet.querySelector('[data-save]').disabled = choice === savedWallpaper;
}

const presetGrid = sheet.querySelector('[data-presets]');
for (const wallpaper of wallpapers) {
  const option = document.createElement('button');
  const picture = document.createElement('img');
  picture.src = wallpaper.url;
  picture.alt = '';
  option.type = 'button';
  option.append(picture, wallpaper.title);
  option.addEventListener('click', () => selectWallpaper(wallpaper));
  presetGrid.append(option);
}
selectWallpaper(savedWallpaper);
renderAccount();

app.querySelectorAll('[data-nav]').forEach(button => button.addEventListener('click', () => navigate(button.dataset.nav)));
app.querySelector('[data-accounts-back]').addEventListener('click', () => {
  navigate('account');
  app.querySelector('[data-nav="accounts"]').focus();
});
sheet.querySelector('[data-add-account]').addEventListener('click', () => {
  clearTimeout(accountSwitchTimer);
  accountSwitchTimer = undefined;
  accountList.removeAttribute('aria-busy');
  const number = profiles.length + 1;
  const profile = {
    name: number === 3 ? '陈宁' : '演示账户 ' + number,
    login: 'demo' + number + '@example.com',
    network: 'tailnet-demo-' + number,
    address: '100.100.1.' + number
  };
  profiles.push(profile);
  activateProfile(profile, '已添加');
});
app.querySelectorAll('[data-back-to-more]').forEach(button => button.addEventListener('click', () => {
  navigate('more');
  app.querySelector('[data-view-panel="more"] [data-nav="' + button.dataset.backToMore + '"]').focus();
}));
app.querySelectorAll('[data-open]').forEach(button => button.addEventListener('click', () => {
  if (button.dataset.device) selectedDevice = button.dataset.device;
  showSheet(button.dataset.open);
}));
function enterStream(watchOnly = false) {
  if (!signedIn || activeProfile !== profiles[0]) return;
  stream.dataset.watch = String(watchOnly);
  sheet.querySelector('[data-stream-status]').textContent = watchOnly ? '观看模式' : '已连接';
  navigate('stream');
}

app.querySelectorAll('[data-connect]').forEach(button => button.addEventListener('click', () => enterStream()));
app.querySelectorAll('[data-device-action]:enabled').forEach(button => button.addEventListener('click', () => {
  const action = button.dataset.deviceAction;
  if (action === '观看模式') enterStream(true);
  else message(action + ' · 原型演示');
}));
app.querySelectorAll('[data-login]').forEach(button => button.addEventListener('click', () => {
  signedIn = true;
  renderAccount();
  navigate('devices');
  message('已登录 Tailscale');
}));
app.querySelector('[data-logout]').addEventListener('click', () => {
  signedIn = false;
  clearTimeout(accountSwitchTimer);
  accountSwitchTimer = undefined;
  accountList.removeAttribute('aria-busy');
  closeSheet();
  renderAccount();
  navigate('devices');
  message('已退出登录');
});
sheet.querySelector('.close-sheet').addEventListener('click', closeSheet);
sheet.querySelectorAll('[data-cancel]').forEach(button => button.addEventListener('click', closeSheet));
sheet.querySelector('[data-disconnect]').addEventListener('click', () => {
  closeSheet();
  navigate('devices');
  message('已断开远控');
});
app.querySelector('[data-account-refresh]').addEventListener('click', event => refreshStatus(event.currentTarget, 'Tailscale 状态已刷新'));
app.querySelector('[data-refresh]').addEventListener('click', event => refreshStatus(event.currentTarget, '设备列表已刷新'));

function refreshStatus(button, done) {
  button.disabled = true;
  button.querySelector('.icon').classList.add('is-refreshing');
  setTimeout(() => {
    button.disabled = false;
    button.querySelector('.icon').classList.remove('is-refreshing');
    message(done);
  }, 450);
}

app.querySelector('[data-account-copy]').addEventListener('click', event => {
  copyAddress(event.currentTarget.previousElementSibling.textContent);
});
sheet.querySelectorAll('[data-copy]').forEach(button => button.addEventListener('click', () => {
  const address = devices[selectedDevice][button.dataset.copy];
  closeSheet();
  copyAddress(address);
}));

async function copyAddress(address) {
  try {
    await navigator.clipboard.writeText(address);
    message('已复制地址');
  } catch {
    message(address);
  }
}
sheet.querySelectorAll('[data-power]').forEach(button => button.addEventListener('click', () => {
  powerAction = button.dataset.power;
  showSheet('device-power');
}));
sheet.querySelector('[data-power-confirm]').addEventListener('click', () => {
  closeSheet();
  message(powerAction + '请求 · 原型演示');
});
app.querySelector('[data-check-update]').addEventListener('click', event => {
  const button = event.currentTarget;
  const label = button.querySelector('span');
  const icon = button.querySelector('.icon');
  button.disabled = true;
  label.textContent = '正在检查…';
  icon.classList.add('is-refreshing');
  setTimeout(() => {
    label.textContent = '检查更新';
    icon.classList.remove('is-refreshing');
    button.disabled = false;
    message('当前已是最新版本', 3000);
  }, 600);
});
for (const kind of ['quality', 'input']) {
  sheet.querySelectorAll('[data-' + kind + ']').forEach(button => button.addEventListener('click', () => {
    setChoice(kind, button.dataset[kind]);
    closeSheet();
  }));
}
sheet.querySelectorAll('[data-display]').forEach(button => button.addEventListener('click', () => {
  const display = button.dataset.display;
  setChoice('display', display);
  image.src = 'assets/wallpapers/default/' + displayPhotos[display] + '.webp';
  image.alt = '书房主机 ' + display + '桌面预览';
  closeSheet();
  message('已切换至' + display);
}));
sheet.querySelectorAll('[data-privacy]').forEach(button => button.addEventListener('click', () => {
  setChoice('privacy', button.dataset.privacy);
  sheet.querySelector('[data-privacy-state]').textContent = button.dataset.privacy;
  closeSheet();
  message(button.dataset.privacy === '关闭' ? '已关闭隐私保护' : '已开启隐私屏保');
}));
app.querySelector('[data-statistics]').addEventListener('change', event => {
  app.querySelector('[data-performance]').hidden = !event.target.checked;
});
sheet.querySelector('[data-send-text]').addEventListener('click', () => {
  const field = sheet.querySelector('textarea');
  if (!field.value.trim()) return;
  field.value = '';
  closeSheet();
  message('输入示例已提交');
});
const upload = sheet.querySelector('[data-picture-file]');
sheet.querySelector('[data-import]').addEventListener('click', () => upload.click());
upload.addEventListener('change', async () => {
  const file = upload.files[0];
  upload.value = '';
  if (!file) return;
  const url = URL.createObjectURL(file);
  const sample = new Image();
  sample.src = url;
  try {
    await sample.decode();
    if (sheet.open) {
      selectWallpaper({ url, title: file.name, custom: true });
      return;
    }
  } catch {
    message('无法预览这张图片');
  }
  URL.revokeObjectURL(url);
});
sheet.querySelector('[data-restore]').addEventListener('click', () => selectWallpaper(wallpapers[0]));
sheet.querySelector('[data-save]').addEventListener('click', () => {
  const previous = savedWallpaper;
  savedWallpaper = draftWallpaper;
  if (previous.custom && previous !== savedWallpaper) URL.revokeObjectURL(previous.url);
  selectWallpaper(savedWallpaper);
  closeSheet();
  message('样式已保存到书房主机');
});
