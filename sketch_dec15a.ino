#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h> 

#define EEPROM_SIZE 4
#define GPIO_UP 25
#define GPIO_DOWN 26

const char* ssid = "Piec_Controller";
const char* password = "12345678";

WebServer server(80);

float lastTemperature = 60.0; 
bool heating = false;
unsigned long heatingStartTime = 0;
unsigned long heatingDuration = 0; 
std::vector<std::pair<float, unsigned long>> programs; 


String htmlPage() {
    String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
        <title>Sterowanie Piecem</title>
        <style>
            body { font-family: Arial, sans-serif; text-align: center; }
            .program { margin: 10px 0; }
            input[type="number"], input[type="time"] { margin: 5px; padding: 10px; width: 100px; }
            button { padding: 10px 20px; margin: 10px; }
        </style>
    </head>
    <body>
        <h1>Sterowanie Piecem</h1>
        <h3>Ostatnia zapisana temperatura: <span id="lastTemp"></span> stopni</h3>
        <h3>Pozostaly czas: <span id="remainingTime"></span> (Aktualna temperatura: <span id="currentTemp"></span> stopni)</h3>
        <div id="programContainer">
            <div class="program">
                <input type="number" placeholder="Temperatura (stopni)" id="tempInput" step="5" min="5">
                <input type="time" id="timeInput">
                <button onclick="addProgram()">Dodaj</button>
            </div>
        </div>
        <button onclick="submitProgram()">Rozpocznij Program</button>
        <button onclick="stopProgram()">Zatrzymaj Piec</button>
        <script>
            let programs = [];
            let remainingTime = 0;
            document.getElementById('lastTemp').innerText = )rawliteral" + String(lastTemperature) + R"rawliteral(;

            function updateRemainingTime(time, temp) {
                const minutes = Math.floor(time / 60000);
                const seconds = Math.floor((time % 60000) / 1000);
                document.getElementById('remainingTime').innerText = minutes + " min " + seconds + " s";
                document.getElementById('currentTemp').innerText = temp.toFixed(1);
            }

            function addProgram() {
                const temp = parseFloat(document.getElementById('tempInput').value);
                const time = document.getElementById('timeInput').value;
                if (!isNaN(temp) && time) {
                    programs.push({ temp, time });
                    refreshProgramList();
                } else {
                    alert("Prosze wprowadzic poprawne dane.");
                }
            }

            function removeProgram(index) {
                programs.splice(index, 1);
                refreshProgramList();
            }

            function refreshProgramList() {
                const programContainer = document.getElementById('programContainer');
                programContainer.innerHTML = 
                    `<div class="program">
                        <input type="number" placeholder="Temperatura (stopni)" id="tempInput" step="5" min="5">
                        <input type="time" id="timeInput">
                        <button onclick="addProgram()">Dodaj</button>
                    </div>`;
                for (let i = 0; i < programs.length; i++) {
                    const program = programs[i];
                    const container = document.createElement('div');
                    container.innerHTML = `<p>${program.temp} stopni przez ${program.time} <button onclick="removeProgram(${i})">Usun</button></p>`;
                    programContainer.appendChild(container);
                }
            }

            function submitProgram() {
                if (programs.length === 0) {
                    alert("Nie dodano zadnego programu.");
                    return;
                }

                const program = programs[0];
                const confirmStart = confirm(
                    `Czy na pewno chcesz uruchomic program: ${program.temp} stopni przez ${program.time}?`
                );

                if (!confirmStart) {
                    return; 
                }

                fetch("/start", {
                    method: "POST",
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(programs)
                })
                .then(() => alert("Program rozpoczety!"))
                .catch(() => alert("Blad podczas wysylania danych!"));
            }

            function stopProgram() {
                const confirmStop = confirm("Czy na pewno chcesz zatrzymac piec?");
                if (confirmStop) {
                    fetch("/stop", { method: "POST" })
                    .then(() => alert("Piec zatrzymany"))
                    .catch(() => alert("Blad podczas wysylania danych"));
                }
            }

            setInterval(function() {
                fetch("/remaining_time")
                    .then(response => response.json())
                    .then(data => {
                        remainingTime = data.remaining_time;
                        updateRemainingTime(remainingTime, data.current_temp || 0);
                    });
            }, 1000);
        </script>
    </body>
    </html>
    )rawliteral";
    return html;
}


void stopHeating() {
    digitalWrite(GPIO_UP, LOW);  
    heating = false;
    heatingDuration = 0;

    Serial.println("Piec zatrzymany.");
}

void handleProgramStop() {
    stopHeating();
    server.send(200, "text/plain", "Piec zatrzymany");
}

unsigned long parseTime(const String& timeStr) {
    int hours, minutes;
    if (sscanf(timeStr.c_str(), "%d:%d", &hours, &minutes) == 2) {
        return (hours * 3600 + minutes * 60) * 1000;
    }
    return 0;
}

void startHeating(float targetTemperature, unsigned long duration) {
    digitalWrite(GPIO_UP, HIGH);  
    heating = true;
    heatingStartTime = millis();
    heatingDuration = duration;

    lastTemperature = targetTemperature;
    EEPROM.put(0, lastTemperature);
    EEPROM.commit();

    Serial.println("Piec uruchomiony: " + String(targetTemperature) + " stopni na " + String(duration / 1000) + " sekund.");
}

void startNextProgram() {
    if (!programs.empty()) {
        float targetTemp = programs[0].first;
        unsigned long duration = programs[0].second;
        programs.erase(programs.begin());
        startHeating(targetTemp, duration);
    }
}

void handleProgramStart() {
    if (server.hasArg("plain")) {
        String body = server.arg("plain");
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, body);
        if (error) {
            server.send(400, "text/plain", "Blad parsowania JSON");
            return;
        }
        programs.clear();
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject obj : arr) {
            programs.push_back({ obj["temp"].as<float>(), parseTime(obj["time"].as<String>()) });
        }

        startNextProgram();
        server.send(200, "text/plain", "Program rozpoczety.");
    } else {
        server.send(400, "text/plain", "Brak danych.");
    }
}

void handleRemainingTime() {
    if (heating) {
        unsigned long remaining = heatingDuration - (millis() - heatingStartTime);
        server.send(200, "application/json", "{\"remaining_time\": " + String(remaining) +
                                              ", \"current_temp\": " + String(lastTemperature) + "}");
    } else {
        server.send(200, "application/json", "{\"remaining_time\": 0, \"current_temp\": 0}");
    }
}

void setup() {
    Serial.begin(115200);
    EEPROM.begin(EEPROM_SIZE);

    pinMode(GPIO_UP, OUTPUT);
    pinMode(GPIO_DOWN, OUTPUT);
    digitalWrite(GPIO_UP, LOW);   
    digitalWrite(GPIO_DOWN, LOW);

    EEPROM.get(0, lastTemperature);

    WiFi.softAP(ssid, password);
    IPAddress IP = WiFi.softAPIP();
    Serial.println("IP Address: " + IP.toString());

    server.on("/", []() {
        server.send(200, "text/html", htmlPage());
    });

    server.on("/start", HTTP_POST, handleProgramStart);
    server.on("/stop", HTTP_POST, handleProgramStop);
    server.on("/remaining_time", HTTP_GET, handleRemainingTime);

    server.begin();
    Serial.println("Serwer uruchomiony.");
}

void loop() {
    server.handleClient();

    if (heating && millis() - heatingStartTime >= heatingDuration) {
        stopHeating();
        startNextProgram(); 
    }
}
