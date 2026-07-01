/*
  LOGICSURU Stepper Car - ESP32-C3 SuperMini
  Two 28BYJ-48 stepper motors (via ULN2003 driver boards) used as drive motors.
  Hosts a WiFi Access Point with a web page to control:
    Forward / Backward / Left / Right / Stop
    Front (white) LED toggle
    Tail (red) LED toggle
    Horn (buzzer + jingle + headlight flash)

  Pin map (as given):
    Left Motor:  IN1=2  IN2=3  IN3=4  IN4=5
    Right Motor: IN1=6  IN2=7  IN3=8  IN4=9
    White LED (front) = 20
    Red LED (tail)    = 21
    Buzzer            = 10
*/

#include <WiFi.h>
#include <WebServer.h>

// ---------- WiFi AP credentials ----------
const char* AP_SSID = "LOGICSURU-CAR";
const char* AP_PASS = "12345678";   // min 8 chars, change if you like

WebServer server(80);

// ---------- Motor pins ----------
const int LEFT_PINS[4]  = {2, 3, 4, 5};
const int RIGHT_PINS[4] = {6, 7, 8, 9};

const int WHITE_LED = 20;
const int RED_LED   = 21;
const int BUZZER     = 10;

// ---------- Half-step sequence for 28BYJ-48 (8 steps, smoother + more torque) ----------
const uint8_t STEP_SEQ[8][4] = {
  {1,0,0,0},
  {1,1,0,0},
  {0,1,0,0},
  {0,1,1,0},
  {0,0,1,0},
  {0,0,1,1},
  {0,0,0,1},c:\Users\SURAJ MARU\Downloads\esp32c3_stepper_car (3).ino
  {1,0,0,1}
};

// direction: 1 = forward, -1 = backward, 0 = stopped
volatile int leftDir  = 0;
volatile int rightDir = 0;

int leftStepIndex  = 0;
int rightStepIndex = 0;

unsigned long lastLeftStepTime  = 0;
unsigned long lastRightStepTime = 0;

// Microseconds between steps -> controls speed. Lower = faster.
// 28BYJ-48 + ULN2003 generally fine around 2-4ms per half-step.
unsigned int stepIntervalUs = 1200;

bool frontLightOn = false;
bool tailLightOn  = false;

// ---------- Horn melody (non-blocking) ----------
// A short original "honk honk" style jingle: {frequency_Hz, duration_ms}
struct Note { int freq; int dur; };
const Note HORN_TUNE[] = {
  {1800, 120}, {0, 40},
  {1800, 120}, {0, 40},
  {2200, 220}, {0, 60},
  {1600, 300}
};
const int HORN_TUNE_LEN = sizeof(HORN_TUNE) / sizeof(Note);

bool melodyPlaying = false;
int  melodyIndex = 0;
unsigned long melodyNoteStart = 0;
bool frontLightPrevState = false; // remembers light state before horn forced it on

void startHornMelody() {
  melodyPlaying = true;
  melodyIndex = 0;
  melodyNoteStart = millis();
  if (HORN_TUNE[0].freq > 0) tone(BUZZER, HORN_TUNE[0].freq);
  else noTone(BUZZER);

  frontLightPrevState = frontLightOn;
  digitalWrite(WHITE_LED, HIGH); // headlight on with horn
}

void stopHornMelody() {
  melodyPlaying = false;
  noTone(BUZZER);
  digitalWrite(BUZZER, LOW);
  digitalWrite(WHITE_LED, frontLightPrevState ? HIGH : LOW); // restore headlight
}

void updateHornMelody() {
  if (!melodyPlaying) return;
  unsigned long now = millis();
  if (now - melodyNoteStart >= (unsigned long)HORN_TUNE[melodyIndex].dur) {
    melodyIndex++;
    if (melodyIndex >= HORN_TUNE_LEN) {
      melodyIndex = 0; // loop the tune while button is held
    }
    melodyNoteStart = now;
    if (HORN_TUNE[melodyIndex].freq > 0) tone(BUZZER, HORN_TUNE[melodyIndex].freq);
    else noTone(BUZZER);
  }
}

// ---------------- Motor helpers ----------------
void writeStep(const int pins[4], int stepIndex) {
  for (int i = 0; i < 4; i++) {
    digitalWrite(pins[i], STEP_SEQ[stepIndex][i]);
  }
}

void updateMotor(const int pins[4], volatile int &dir, int &stepIndex, unsigned long &lastStepTime) {
  if (dir == 0) return; // stopped, do nothing
  unsigned long now = micros();
  if (now - lastStepTime >= stepIntervalUs) {
    lastStepTime = now;
    stepIndex += dir;
    if (stepIndex > 7) stepIndex = 0;
    if (stepIndex < 0) stepIndex = 7;
    writeStep(pins, stepIndex);
  }
}

void stopAllMotors() {
  leftDir = 0;
  rightDir = 0;
  // de-energize coils so motors don't sit there heating up
  for (int i = 0; i < 4; i++) {
    digitalWrite(LEFT_PINS[i], LOW);
    digitalWrite(RIGHT_PINS[i], LOW);
  }
}

// ---------------- Web page ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LOGICSURU CAR</title>
<style>
  body{
    background:#0a0a0a;color:#eee;font-family:Arial,Helvetica,sans-serif;
    text-align:center;margin:0;padding:20px;
  }
  h1{color:#fff;letter-spacing:2px;font-size:22px;margin-bottom:4px;}
  .sub{color:#888;font-size:12px;margin-bottom:20px;}
  .pad{
    display:grid;
    grid-template-columns:80px 80px 80px;
    grid-template-rows:80px 80px 80px;
    gap:10px;justify-content:center;margin:20px auto;
  }
  button{
    border:none;border-radius:12px;background:#1c1c1c;color:#fff;
    font-size:24px;box-shadow:0 0 0 1px #333 inset;
  }
  button:active{background:#4f46e5;}
  .stop{background:#7a1212;}
  .stop:active{background:#b91c1c;}
  .row{display:flex;justify-content:center;gap:14px;margin-top:25px;flex-wrap:wrap;}
  .toggle{
    width:120px;height:60px;border-radius:10px;font-size:14px;
    background:#1c1c1c;color:#fff;box-shadow:0 0 0 1px #333 inset;
  }
  .toggle.on{background:#16a34a;}
  .horn{background:#92400e;}
  .horn:active{background:#d97706;}
</style>
</head>
<body>
  <h1>LOGICSURU CAR</h1>
  <div class="sub">ESP32-C3 Stepper Drive</div>

  <div class="pad">
    <div></div>
    <button ontouchstart="cmd('forward')" ontouchend="cmd('stop')" onmousedown="cmd('forward')" onmouseup="cmd('stop')">&#8593;</button>
    <div></div>

    <button ontouchstart="cmd('left')" ontouchend="cmd('stop')" onmousedown="cmd('left')" onmouseup="cmd('stop')">&#8592;</button>
    <button class="stop" onclick="cmd('stop')">&#9632;</button>
    <button ontouchstart="cmd('right')" ontouchend="cmd('stop')" onmousedown="cmd('right')" onmouseup="cmd('stop')">&#8594;</button>

    <div></div>
    <button ontouchstart="cmd('backward')" ontouchend="cmd('stop')" onmousedown="cmd('backward')" onmouseup="cmd('stop')">&#8595;</button>
    <div></div>
  </div>

  <div class="row">
    <button class="toggle" id="frontBtn" onclick="toggle('front')">FRONT LIGHT</button>
    <button class="toggle" id="tailBtn" onclick="toggle('tail')">TAIL LIGHT</button>
    <button class="toggle horn" ontouchstart="cmd('hornon')" ontouchend="cmd('hornoff')" onmousedown="cmd('hornon')" onmouseup="cmd('hornoff')">HORN</button>
  </div>

<script>
function cmd(c){
  fetch('/' + c).catch(()=>{});
}
function toggle(which){
  let btn = document.getElementById(which === 'front' ? 'frontBtn' : 'tailBtn');
  let isOn = btn.classList.contains('on');
  let endpoint = which + (isOn ? 'off' : 'on');
  fetch('/' + endpoint).then(()=>{
    btn.classList.toggle('on');
  }).catch(()=>{});
}
// prevent scrolling/zooming weirdness on touch
document.addEventListener('touchmove', function(e){ e.preventDefault(); }, {passive:false});
</script>
</body>
</html>
)rawliteral";

// ---------------- Route handlers ----------------
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleForward()  { leftDir = 1;  rightDir = -1; server.send(200, "text/plain", "OK"); }
void handleBackward() { leftDir = -1; rightDir = 1;  server.send(200, "text/plain", "OK"); }
void handleLeft()     { leftDir = -1; rightDir = -1; server.send(200, "text/plain", "OK"); } // pivot left
void handleRight()    { leftDir = 1;  rightDir = 1;  server.send(200, "text/plain", "OK"); } // pivot right
void handleStop()     { stopAllMotors(); server.send(200, "text/plain", "OK"); }

void handleFrontOn()  { frontLightOn = true;  digitalWrite(WHITE_LED, HIGH); server.send(200, "text/plain", "OK"); }
void handleFrontOff() { frontLightOn = false; digitalWrite(WHITE_LED, LOW);  server.send(200, "text/plain", "OK"); }
void handleTailOn()   { tailLightOn = true;   digitalWrite(RED_LED, HIGH);   server.send(200, "text/plain", "OK"); }
void handleTailOff()  { tailLightOn = false;  digitalWrite(RED_LED, LOW);    server.send(200, "text/plain", "OK"); }

void handleHornOn()  { startHornMelody(); server.send(200, "text/plain", "OK"); }
void handleHornOff() { stopHornMelody();  server.send(200, "text/plain", "OK"); }

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 4; i++) {
    pinMode(LEFT_PINS[i], OUTPUT);
    pinMode(RIGHT_PINS[i], OUTPUT);
    digitalWrite(LEFT_PINS[i], LOW);
    digitalWrite(RIGHT_PINS[i], LOW);
  }
  pinMode(WHITE_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(WHITE_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER, LOW);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP()); // usually 192.168.4.1

  server.on("/", handleRoot);
  server.on("/forward", handleForward);
  server.on("/backward", handleBackward);
  server.on("/left", handleLeft);
  server.on("/right", handleRight);
  server.on("/stop", handleStop);
  server.on("/fronton", handleFrontOn);
  server.on("/frontoff", handleFrontOff);
  server.on("/tailon", handleTailOn);
  server.on("/tailoff", handleTailOff);
  server.on("/hornon", handleHornOn);
  server.on("/hornoff", handleHornOff);

  server.begin();
  Serial.println("Server started");
}

// ---------------- Loop ----------------
void loop() {
  server.handleClient();
  updateMotor(LEFT_PINS, leftDir, leftStepIndex, lastLeftStepTime);
  updateMotor(RIGHT_PINS, rightDir, rightStepIndex, lastRightStepTime);
  updateHornMelody();
}
