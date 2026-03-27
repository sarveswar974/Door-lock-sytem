// MQTT Configuration with auto-protocol detection
const mqttProtocol = location.protocol === 'https:' ? 'wss' : 'ws';
const mqttPort = location.protocol === 'https:' ? '8884' : '8000';
const mqttBroker = `${mqttProtocol}://broker.hivemq.com:${mqttPort}/mqtt`;
const STATUS_TOPIC = 'doorlock/status';
const COMMAND_TOPIC = 'doorlock/command';
const ALERT_TOPIC = 'doorlock/alert';
const options = { clientId: 'web_doorlock_' + Math.random().toString(16).substr(2, 8), clean: true };

let client = null;
let isConnected = false;
let lastHeartbeat = null;
let hardwareCheckInterval = null;

document.addEventListener('DOMContentLoaded', () => { 
    initMQTT();
    
    // Keyboard support for Keypad
    document.addEventListener('keydown', (e) => {
        const keypad = document.getElementById('keypadOverlay');
        if (keypad.style.display === 'flex') {
            if (e.key >= '0' && e.key <= '9') pressKey(e.key);
            else if (e.key === 'Backspace') clearKeypad();
            else if (e.key === 'Enter') submitPin();
            else if (e.key === 'Escape') hideKeypad();
        }
    });
});

// --- Keypad Logic ---
let keypadInput = '';

function showKeypad() { document.getElementById('keypadOverlay').style.display = 'flex'; clearKeypad(); }
function hideKeypad() { document.getElementById('keypadOverlay').style.display = 'none'; }
function pressKey(num) { if (keypadInput.length < 6) { keypadInput += num; updateKeypadDisplay(); } }
function clearKeypad() { keypadInput = ''; updateKeypadDisplay(); }
function updateKeypadDisplay() { document.getElementById('keypadDisplay').innerText = '*'.repeat(keypadInput.length); }

async function submitPin() {
    if (!keypadInput) return;
    const response = await fetch('/api/verify-pin', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ pin: keypadInput })
    });
    const data = await response.json();
    if (data.success) {
        hideKeypad();
        sendCommand('UNLOCK');
        updateDoorStatus('UNLOCKED');
        addLog('Keypad: Access Granted. Unlocking...', 'success');
    } else {
        addLog(`Keypad: Access Denied - ${data.message || 'Incorrect PIN'}`, 'alert');
        alert(data.message || 'Incorrect PIN!');
        clearKeypad();
    }
}

// --- QR Code Logic ---
async function showQRCode() {
    const overlay = document.getElementById('qrOverlay');
    const container = document.getElementById('qrContainer');
    
    overlay.style.display = 'flex';
    container.innerHTML = '<div class="pulse-dot"></div><p style="margin-left: 10px">Generating...</p>';
    
    try {
        const response = await fetch('/api/qrcode');
        if (response.ok) {
            const svgContent = await response.text();
            container.innerHTML = svgContent;
        } else {
            container.innerHTML = '<p style="color: #ef4444">Failed to load QR code</p>';
        }
    } catch (e) {
        container.innerHTML = '<p style="color: #ef4444">Error connecting to server</p>';
    }
}

function hideQRCode() {
    document.getElementById('qrOverlay').style.display = 'none';
}

async function changeDoorPin() {
    const newPin = document.getElementById('newDoorPin').value;
    if (newPin.length < 4) return alert("PIN must be at least 4 digits.");
    
    const response = await fetch('/api/set-pin', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ new_pin: newPin })
    });
    const data = await response.json();
    alert(data.message);
    if (data.success) { document.getElementById('newDoorPin').value = ''; addLog('Admin updated the Door PIN', 'info'); }
}

// --- MQTT Logic ---
function initMQTT() {
    client = mqtt.connect(mqttBroker, options);

    client.on('connect', () => {
        isConnected = true;
        document.getElementById('connectionStatus').innerHTML = '<span class="pulse-dot" style="background: #10b981;"></span> Connected';
        client.subscribe(STATUS_TOPIC);
        client.subscribe(ALERT_TOPIC);
        addLog('Connected to Smart Lock System');
    });
    client.on('message', (topic, message) => {
        try {
            if (topic === STATUS_TOPIC) {
                const data = JSON.parse(message.toString());
                updateDoorStatus(data.door_status);
                lastHeartbeat = Date.now();
                updateHardwareStatus(true);
            } else if (topic === ALERT_TOPIC) {
                const data = JSON.parse(message.toString());
                if (data.type === 'alert') {
                    addLog(`Hardware: ${data.message}`, 'alert');
                } else if (data.type === 'password_changed') {
                    addLog(`Hardware: Global Door PIN changed successfully`, 'success');
                }
            }
        } catch (e) {
            console.error("MQTT Parse Error:", e);
        }
    });
    
    // Start hardware heartbeat monitor
    if (!hardwareCheckInterval) {
        hardwareCheckInterval = setInterval(() => {
            if (lastHeartbeat !== null && (Date.now() - lastHeartbeat > 15000)) { // 15 seconds timeout
                updateHardwareStatus(false);
            } else if (lastHeartbeat === null) {
                updateHardwareStatus(false); // Shows "Synchronizing..."
            }
        }, 5000);
    }
}

function updateHardwareStatus(online) {
    const statusEl = document.getElementById('connectionStatus');
    if (online) {
        statusEl.innerHTML = '<span class="pulse-dot" style="background: #10b981; box-shadow: 0 0 10px #10b981;"></span> Lock Online';
    } else {
        if (lastHeartbeat === null) {
            statusEl.innerHTML = '<span class="pulse-dot" style="background: #f59e0b; box-shadow: 0 0 10px #f59e0b;"></span> Synchronizing...';
        } else {
            statusEl.innerHTML = '<span class="pulse-dot" style="background: #ef4444; box-shadow: 0 0 10px #ef4444;"></span> Lock Offline';
        }
    }
}

function sendCommand(command) {
    if (!isConnected) return alert('Not connected to lock!');
    client.publish(COMMAND_TOPIC, command);
    addLog(`Command sent: ${command}`);
}

function updateDoorStatus(status) {
    const icon = document.getElementById('doorIcon');
    const text = document.getElementById('doorText');
    const display = document.getElementById('doorStatus');
    
    if (status === 'UNLOCKED') {
        icon.className = 'fa-solid fa-unlock'; text.innerHTML = 'UNLOCKED';
        display.className = 'door-status-display unlocked';
    } else {
        icon.className = 'fa-solid fa-lock'; text.innerHTML = 'LOCKED';
        display.className = 'door-status-display locked';
    }
}

function addLog(message, type = 'info') {
    const container = document.getElementById('logContainer');
    if (!container) return;
    const time = new Date().toLocaleTimeString();
    const entry = document.createElement('div');
    let iconClass = type === 'success' ? 'fa-check-circle' : type === 'alert' ? 'fa-triangle-exclamation' : 'fa-info-circle';
    entry.className = `log-entry ${type}`;
    entry.innerHTML = `<i class="fa-solid ${iconClass}"></i> <span class="log-time">[${time}]</span> ${message}`;
    container.prepend(entry);
    if (container.children.length > 50) container.lastChild.remove();
}
