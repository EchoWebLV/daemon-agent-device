// ──── Dev hot-reload ────
// Polls a build-time timestamp file. When Vite rebuilds, the timestamp changes
// and the extension auto-reloads. Safe to leave in — does nothing when files
// are static (published extension).
let _prevStamp = '';
async function checkForReload() {
  try {
    const r = await fetch(chrome.runtime.getURL('hot-reload.stamp'));
    const stamp = await r.text();
    if (_prevStamp && stamp !== _prevStamp) chrome.runtime.reload();
    _prevStamp = stamp;
  } catch {
    // file doesn't exist in production — silently ignore
  }
  setTimeout(checkForReload, 1500);
}
checkForReload();

// ──── Main logic ────
const BALANCE_ALARM = 'balance-check';

chrome.runtime.onInstalled.addListener(() => {
  chrome.alarms.create(BALANCE_ALARM, { periodInMinutes: 5 });
  chrome.sidePanel.setPanelBehavior({ openPanelOnActionClick: false });
});

chrome.alarms.onAlarm.addListener(async (alarm) => {
  if (alarm.name !== BALANCE_ALARM) return;
  try {
    const result = await chrome.storage.local.get('usdc_balance');
    const balance = result.usdc_balance;
    if (typeof balance === 'number') {
      const text = balance >= 1 ? `$${Math.floor(balance)}` : balance > 0 ? '<$1' : '';
      chrome.action.setBadgeText({ text });
      chrome.action.setBadgeBackgroundColor({ color: '#1361f7' });
    }
  } catch {
    // storage read failure — ignore
  }
});

chrome.storage.onChanged.addListener((changes, area) => {
  if (area !== 'local') return;
  if (changes.usdc_balance) {
    const balance = changes.usdc_balance.newValue;
    if (typeof balance === 'number') {
      const text = balance >= 1 ? `$${Math.floor(balance)}` : balance > 0 ? '<$1' : '';
      chrome.action.setBadgeText({ text });
      chrome.action.setBadgeBackgroundColor({ color: '#1361f7' });
    }
  }
});
