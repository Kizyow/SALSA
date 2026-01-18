from flask import Flask, request, jsonify
from flask_cors import CORS
from datetime import datetime
import time
import requests
import json
import os
import uuid

app = Flask(__name__)
CORS(app)

API_KEY = "OPENWEATHERMAP_API_KEY" 

weather_cache = None       
last_fetch_time = 0       
CACHE_DURATION = 30 * 60 

PLANNING_FILE = "planning.json"
CONFIG_FILE = "config.json"
ARDUINO_IP = "X.X.X.X"

@app.route('/trigger', methods=['GET'])
def remote_trigger():
    global ARDUINO_IP
    
    if ARDUINO_IP is None:
        return jsonify({"error": "Arduino introuvable. Attendez qu'il contacte le serveur."}), 503
    
    try:
        action = request.args.get('go') 
        target_url = f"http://{ARDUINO_IP}/trigger"
        
        if action:
            target_url += f"?go={action}"

        print(f"Relai de commande vers : {target_url}")
        
        resp = requests.get(target_url, timeout=15)
        
        return jsonify({
            "status": "success", 
            "arduino_response": resp.text,
            "arduino_ip": ARDUINO_IP
        })
        
    except Exception as e:
        print(f"Erreur de communication avec Arduino: {e}")
        return jsonify({"error": "Impossible de joindre l'Arduino", "details": str(e)}), 500

def load_config():
    default_config = {
        "inverser_ordre": False, 
        "mode_manuel": True,
        "is_opened": False,
        "seuil_temp_min": 10.0,
        "seuil_temp_max": 25.0,
        "city": "Toulouse"
    }
    if not os.path.exists(CONFIG_FILE):
        with open(CONFIG_FILE, 'w') as f:
            json.dump(default_config, f)
        return default_config
        
    try:
        with open(CONFIG_FILE, 'r') as f:
            return json.load(f)
    except:
        return default_config

def save_config(conf):
    with open(CONFIG_FILE, 'w') as f:
        json.dump(conf, f, indent=4)

@app.route('/config', methods=['GET'])
def get_config():
    conf = load_config()
    return jsonify(conf)

@app.route('/config/update', methods=['POST'])
def update_config():
    global weather_cache
    data = request.json
    conf = load_config()
    changed = False

    if 'inverser_ordre' in data:
        conf['inverser_ordre'] = bool(data['inverser_ordre'])
        changed = True
        
    if 'mode_manuel' in data:
        conf['mode_manuel'] = bool(data['mode_manuel'])
        changed = True

    if 'is_opened' in data:
        conf['is_opened'] = bool(data['is_opened'])
        changed = True

    if 'seuil_temp_min' in data:
        try:
            conf['seuil_temp_min'] = float(data['seuil_temp_min'])
            changed = True
        except ValueError:
            pass

    if 'seuil_temp_max' in data:
        try:
            conf['seuil_temp_max'] = float(data['seuil_temp_max'])
            changed = True
        except ValueError:
            pass

    if 'city' in data:
        new_city = data['city'].strip()
        if new_city:
            conf['city'] = new_city
            changed = True
            print(f"Changement de ville : {new_city}")
            weather_cache = None

    if changed:
        save_config(conf)
        
    return jsonify({"status": "updated", "config": conf})

def load_planning():
    if not os.path.exists(PLANNING_FILE):
        return []
    try:
        with open(PLANNING_FILE, 'r') as f:
            return json.load(f)
    except:
        return []

def save_planning(planning_data):
    with open(PLANNING_FILE, 'w') as f:
        json.dump(planning_data, f, indent=4)

@app.route('/planning/add', methods=['POST'])
def add_event():
    data = request.json
    
    new_event = {
        "id": str(uuid.uuid4()),
        "target_datetime": f"{data['date']} {data['time']}",
        "action": data['action'], 
        "executed": False
    }
    
    events = load_planning()
    events.append(new_event)
    save_planning(events)
    
    print(f"Nouvel événement planifié : {new_event['target_datetime']} -> {new_event['action']}")
    return jsonify({"status": "ok", "event": new_event})

@app.route('/planning/list', methods=['GET'])
def list_events():
    events = load_planning()
    events.sort(key=lambda x: x['target_datetime'])
    return jsonify(events)

@app.route('/planning/check', methods=['GET'])
def check_schedule():
    events = load_planning()
    now = datetime.now()
    
    action_to_do = "AUCUNE"
    modified = False
    
    for event in events:
        if not event['executed']:
            event_dt = datetime.strptime(event['target_datetime'], "%Y-%m-%d %H:%M")
            
            if now >= event_dt:
                action_to_do = event['action']
                event['executed'] = True
                modified = True
                print(f"EXECUTION PLANIFIEE : {action_to_do} (Prevue a {event['target_datetime']})")
                break 
    
    if modified:
        save_planning(events)
        
    return jsonify({
        "execute": action_to_do
    })

@app.route('/planning/cleanup', methods=['GET'])
def cleanup_events():
    events = load_planning()
    new_list = [e for e in events if not e['executed']]
    save_planning(new_list)
    return jsonify({"message": "Nettoyage termine", "restants": len(new_list)})

@app.route('/planning/clear_future', methods=['POST'])
def clear_future_events():
    events = load_planning()
    now = datetime.now()
    
    kept_events = []
    for e in events:
        try:
            event_dt = datetime.strptime(e['target_datetime'], "%Y-%m-%d %H:%M")
            if event_dt < now or e['executed']:
                kept_events.append(e)
        except:
            pass
            
    save_planning(kept_events)
    return jsonify({"status": "cleared", "count": len(events) - len(kept_events)})

@app.route('/meteo', methods=['GET'])
def get_weather():
    global weather_cache, last_fetch_time

    current_time = time.time()
    
    # caching pour pas se faire ban de openweathermap
    if weather_cache is not None and (current_time - last_fetch_time < CACHE_DURATION):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] CACHE HIT : Envoi des données en mémoire.")
        return jsonify(weather_cache)

    try:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] CACHE MISS : Appel API OpenWeatherMap...")
        
        conf = load_config()
        current_city = conf['city']
        url = f"http://api.openweathermap.org/data/2.5/weather?q={current_city}&appid={API_KEY}&units=metric&lang=fr"
        r = requests.get(url, timeout=10)
        r.raise_for_status()
        data = r.json()
        
        weather_info = {
            "temp": data["main"]["temp"],
            "condition": data["weather"][0]["description"],
            "pluie": "Rain" in data.get("weather", [{}])[0].get("main", "") or "rain" in data
        }
        
        weather_cache = weather_info
        last_fetch_time = current_time
        
        return jsonify(weather_info)
        
    except Exception as e:
        print(f"Erreur lors de l'appel API : {e}")
        
        if weather_cache is not None:
            print("Utilisation du vieux cache de secours.")
            return jsonify(weather_cache)
            
        return jsonify({"error": str(e)}), 500

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)