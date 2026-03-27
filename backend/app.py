import os
import secrets
import time
from dotenv import load_dotenv
from flask import Flask, request, jsonify, send_from_directory, session
from flask_sqlalchemy import SQLAlchemy
from flask_cors import CORS
import paho.mqtt.client as mqtt

import socket
import io

# Load environment variables
load_dotenv()

def get_local_ip():
    """Detect the server's local network IP address."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # doesn't even have to be reachable
        s.connect(('8.8.8.8', 1))
        IP = s.getsockname()[0]
    except Exception:
        IP = '127.0.0.1'
    finally:
        s.close()
    return IP

app = Flask(__name__)
CORS(app)
app.secret_key = os.getenv("FLASK_SECRET_KEY", secrets.token_hex(16))

# ---------- System Configuration ----------
# Correctly resolve the frontend directory relative to this script
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
FRONTEND_DIR = os.path.abspath(os.path.join(BASE_DIR, '../frontend'))
DEFAULT_DOOR_PIN = "1234"

# ---------- Database Configuration ----------
DB_NAME = os.getenv("DATABASE_URL", "users.db")
if not DB_NAME.startswith('sqlite:///'):
    db_path = os.path.join(BASE_DIR, DB_NAME)
    app.config['SQLALCHEMY_DATABASE_URI'] = f"sqlite:///{db_path}"
else:
    app.config['SQLALCHEMY_DATABASE_URI'] = DB_NAME

app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False
db = SQLAlchemy(app)

class Setting(db.Model):
    __tablename__ = 'settings'
    key = db.Column(db.String(50), primary_key=True)
    value = db.Column(db.String(255), nullable=False)

def init_db():
    try:
        with app.app_context():
            db.create_all()
            # Ensure default PIN exists
            if not db.session.get(Setting, 'door_pin'):
                db.session.add(Setting(key='door_pin', value=DEFAULT_DOOR_PIN))
            db.session.commit()
            print("Database initialized successfully.")
    except Exception as e:
        print(f"Database Initialization Error: {e}")

# ---------- MQTT Setup ----------
MQTT_BROKER = os.getenv("MQTT_BROKER", "broker.hivemq.com")
try:
    MQTT_PORT = int(os.getenv("MQTT_PORT", 1883))
except ValueError:
    MQTT_PORT = 1883

LOCK_COMMAND_TOPIC = "doorlock/command"
LOCK_STATUS_TOPIC = "doorlock/status"
LOCK_PASSWORD_TOPIC = "doorlock/password"

# Handle Paho MQTT v1 and v2 compatibility
try:
    from paho.mqtt.enums import CallbackAPIVersion
    mqtt_client = mqtt.Client(CallbackAPIVersion.VERSION2, client_id=f"FlaskBackend_{secrets.token_hex(4)}")
except (ImportError, AttributeError):
    # Fallback for Paho MQTT v1.x or older 2.x versions
    mqtt_client = mqtt.Client(client_id=f"FlaskBackend_{secrets.token_hex(4)}")

def on_connect(client, userdata, flags, rc, properties=None):
    """Callback for when the client receives a CONNACK response from the server."""
    print(f"Connected to MQTT Broker (Status Code: {rc})")
    client.subscribe(LOCK_STATUS_TOPIC)

def on_publish(client, userdata, mid, reason_code=None, properties=None):
    """Callback for when a message is published."""
    # This works for both Paho v1 and v2 due to default arguments
    print(f"MQTT Message #{mid} successfully published.")

mqtt_client.on_connect = on_connect
mqtt_client.on_publish = on_publish

# ---------- Helper Functions ----------
def log_activity(action, method, result):
    print(f"[ACTIVITY] {action} via {method}: {result}")

# ---------- API Routes ----------
@app.route('/api/verify-pin', methods=['POST'])
def verify_pin():
    data = request.get_json(silent=True) or {}
    entered_pin = data.get('pin')
    
    try:
        pin_setting = db.session.get(Setting, 'door_pin')
        real_pin = pin_setting.value if pin_setting else '1234'
        
        if entered_pin == real_pin:
            mqtt_client.publish(LOCK_COMMAND_TOPIC, "UNLOCK")
            log_activity('UNLOCK', 'PIN', 'SUCCESS')
            return jsonify({"success": True, "message": "Door Unlocked!"})
            
        log_activity('UNLOCK', 'PIN', 'DENIED')
        return jsonify({"success": False, "message": "Incorrect PIN"}), 403
    except Exception as e:
        print(f"Verify PIN Error: {e}")
        return jsonify({"success": False, "message": "Server Error"}), 500

@app.route('/api/set-pin', methods=['POST'])
def set_pin():
    data = request.get_json(silent=True) or {}
    new_pin = data.get('new_pin')
    
    if not new_pin or len(new_pin) < 4: 
        return jsonify({"success": False, "message": "PIN must be 4+ digits"}), 400
    
    try:
        pin_setting = db.session.get(Setting, 'door_pin')
        if pin_setting:
            pin_setting.value = new_pin
        else:
            db.session.add(Setting(key='door_pin', value=new_pin))
        db.session.commit()
        
        # Sync with Hardware via MQTT
        mqtt_client.publish(LOCK_PASSWORD_TOPIC, new_pin)
        log_activity('PIN_CHANGE', 'WEB', 'SUCCESS')
        return jsonify({"success": True, "message": "Door PIN updated and synced with hardware"})
        
    except Exception as e:
        db.session.rollback()
        return jsonify({"success": False, "message": str(e)}), 500


@app.route('/api/qrcode')
def get_qrcode():
    """Generate a QR code pointing to the server's local URL."""
    try:
        import qrcode
        import qrcode.image.svg
        
        local_ip = get_local_ip()
        url = f"http://{local_ip}:5000"
            
        # Create QR code
        factory = qrcode.image.svg.SvgPathImage
        img = qrcode.make(url, image_factory=factory, border=2)
        
        # Save to buffer
        stream = io.BytesIO()
        img.save(stream)
        
        return stream.getvalue(), 200, {
            'Content-Type': 'image/svg+xml',
            'Cache-Control': 'no-cache'
        }
    except Exception as e:
        print(f"QR Generation Error: {e}")
        return jsonify({"success": False, "message": "Could not generate QR code"}), 500

# ---------- Static File Serving ----------
@app.route('/')
def index():
    return send_from_directory(FRONTEND_DIR, 'index.html')

@app.route('/<path:path>')
def static_proxy(path):
    return send_from_directory(FRONTEND_DIR, path)

# ---------- App Startup ----------
if __name__ == '__main__':
    # Initialize database
    init_db()
    
    # Start MQTT connection
    try:
        print(f"Connecting to MQTT Broker: {MQTT_BROKER}:{MQTT_PORT}...")
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
        mqtt_client.loop_start()
    except Exception as e:
        print(f"MQTT Initialization Error: {e}")
        
    print(f"\nSmart Door Lock Backend Running on http://localhost:5000")
    print(f"Serving frontend from: {FRONTEND_DIR}")
    
    try:
        app.run(host='0.0.0.0', port=5000, debug=False)
    except Exception as e:
        print(f"Flask Server Error: {e}")
