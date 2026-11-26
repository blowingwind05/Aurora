const editor = document.querySelector('.privacy-editor');
const scenario = preview.querySelector('[data-preview-privacy]');
const safety = menu.querySelector('[data-control-view="safety"]');
const modeButtons = [...safety.querySelectorAll('[data-privacy-mode]')];
const openEditor = safety.querySelector('[data-privacy-open]');
const presetList = editor.querySelector('[data-privacy-presets]');
const upload = editor.querySelector('[data-privacy-file]');
const chooseFile = editor.querySelector('[data-privacy-import]');
const fileName = editor.querySelector('[data-privacy-file-name]');
const restore = editor.querySelector('[data-privacy-reset]');
const enlarge = editor.querySelector('[data-privacy-large]');
const save = editor.querySelector('[data-privacy-save]');
const closeEditor = editor.querySelector('[data-privacy-close]');
const feedback = editor.querySelector('[data-privacy-feedback]');
const choices = [
  ['mountain-valley', '晴空山谷'], ['forest-path', '林间小径'],
  ['blossom-grove', '花树下'], ['ginkgo-lakeside', '银杏湖畔'],
  ['baikal-piano', '冰湖钢琴'], ['baikal-accordion', '冰湖手风琴'],
  ['sunflower-field', '向日葵花田'], ['sunset-sky', '逆光云海']
].map(([id, title]) => ({ title, url: 'assets/wallpapers/default/' + id + '.webp' }));
const defaultChoice = choices[0];
const modeLabels = { off: '关闭', 'display-off': '显示器已关闭', screensaver: '隐私屏保已开启' };

let saved = defaultChoice;
let draft = defaultChoice;
let mode = 'off';
let switching = null;
let saving = false;

for (const choice of choices) {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'privacy-preset';
  const picture = document.createElement('img');
  picture.src = choice.url;
  picture.alt = '';
  const title = document.createElement('span');
  title.textContent = choice.title;
  button.append(picture, title);
  button.addEventListener('click', () => select(choice));
  presetList.append(button);
}

function renderSafety() {
  const supported = scenario.value !== 'unsupported';
  const editable = supported && scenario.value !== 'legacy';
  const active = !session.hidden && session.dataset.phase === 'connected';
  for (const button of modeButtons) {
    button.disabled = !active || !supported || switching !== null;
    button.setAttribute('aria-pressed', String(supported && button.dataset.privacyMode === mode));
  }
  openEditor.disabled = !active || !editable || switching !== null;
  safety.querySelector('[data-privacy-state]').textContent = !supported ? '不可用'
    : switching !== null ? '正在切换…' : modeLabels[mode];
  safety.querySelector('[data-privacy-capability]').textContent = !supported ? '当前被控端未提供隐私保护。'
    : !editable ? '被控端暂不支持自定义屏保。' : '选择模式后立即切换书房主机的隐私保护。';
  const savedLine = safety.querySelector('[data-privacy-saved]');
  savedLine.hidden = !editable;
  savedLine.textContent = '已保存样式 · ' + saved.title;
}

function notice(message = '', error = false) {
  feedback.hidden = !message;
  feedback.textContent = message;
  feedback.classList.toggle('is-error', error);
}

function renderEditor() {
  editor.querySelector('[data-privacy-choice]').textContent = draft.title;
  const image = editor.querySelector('[data-privacy-image]');
  image.src = draft.url;
  editor.querySelector('[data-privacy-apply-rule]').textContent = mode === 'screensaver'
    ? '被控端当前已遮挡。保存确认后将换用这张图片。'
    : '保存样式后，下次开启隐私屏保时使用。';
  fileName.hidden = !draft.custom;
  fileName.textContent = draft.custom ? draft.title : '';
  const locked = saving || session.hidden;
  [...presetList.children].forEach((button, index) => {
    button.disabled = locked;
    button.setAttribute('aria-pressed', String(choices[index] === draft));
  });
  closeEditor.disabled = saving;
  chooseFile.disabled = locked;
  restore.disabled = locked || draft === defaultChoice;
  enlarge.disabled = locked;
  save.disabled = locked || draft === saved;
  save.setAttribute('aria-busy', String(saving));
  save.querySelector('i').hidden = !saving;
  save.querySelector('span').textContent = saving ? '正在保存…' : '保存样式';
}

function select(choice) {
  const old = draft;
  draft = choice;
  if (old.custom && old !== saved && old !== draft) URL.revokeObjectURL(old.url);
  notice();
  renderEditor();
}

for (const button of modeButtons) {
  button.addEventListener('click', () => {
    const next = button.dataset.privacyMode;
    if (mode === next) return;
    switching = setTimeout(() => {
      mode = next;
      switching = null;
      renderSafety();
    }, 500);
    renderSafety();
  });
}

openEditor.addEventListener('click', () => {
  closePopover();
  editor.dataset.large = 'false';
  enlarge.textContent = '大图预览';
  select(saved);
  editor.showModal();
});

closeEditor.addEventListener('click', () => editor.close());
editor.addEventListener('cancel', event => {
  if (saving || editor.dataset.large === 'true') {
    event.preventDefault();
    if (!saving) enlarge.click();
  }
});
editor.addEventListener('close', () => {
  select(saved);
  if (session.hidden) {
    reopen.querySelector('button').focus({ preventScroll: true });
  } else {
    showControlView('safety');
    togglePopover(controls, menu, openEditor);
  }
});

enlarge.addEventListener('click', () => {
  const large = editor.dataset.large !== 'true';
  editor.dataset.large = String(large);
  enlarge.textContent = large ? '返回设置' : '大图预览';
});
restore.addEventListener('click', () => select(defaultChoice));
chooseFile.addEventListener('click', () => upload.click());
upload.addEventListener('change', async () => {
  const file = upload.files[0];
  upload.value = '';
  if (!file) return;
  const url = URL.createObjectURL(file);
  const image = new Image();
  image.src = url;
  try {
    await image.decode();
    if (editor.open && !saving && !session.hidden) {
      select({ title: file.name, url, custom: true });
      return;
    }
  } catch {
    notice('无法预览这张图片，请重新选择。', true);
  }
  URL.revokeObjectURL(url);
});

save.addEventListener('click', () => {
  saving = true;
  notice('正在同步到书房主机…');
  renderEditor();
  setTimeout(() => {
    saving = false;
    if (scenario.value === 'offline') {
      hideWindow('书房主机已离线。重连后可核对已保存样式。');
      notice('连接已断开，保存结果待确认。', true);
    } else if (scenario.value === 'failed') {
      notice('书房主机未能保存样式，请重试。', true);
    } else {
      const previous = saved;
      saved = draft;
      if (previous.custom && previous !== saved) URL.revokeObjectURL(previous.url);
      notice('已保存到书房主机');
      renderSafety();
    }
    renderEditor();
  }, 800);
});

scenario.addEventListener('change', renderSafety);
session.addEventListener('preview-session-closed', () => {
  clearTimeout(switching);
  switching = null;
  mode = 'off';
  renderSafety();
});
new MutationObserver(renderSafety).observe(session, { attributes: true, attributeFilter: ['hidden', 'data-phase'] });
renderSafety();
