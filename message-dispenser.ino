#include <Arduino.h>

// >>> ORDONNANCE FS / WIFI / WEB (ESP32 core 3.x) <<<
#include <FS.h>        // doit être AVANT WebServer.h
using fs::FS;
using fs::File;
#include <SPIFFS.h>
#include <WiFi.h>
#include <WebServer.h>

// --- UI / Touch ---
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <vector>
#include "deja_vu_sans.c"   // expose lv_font_t deja_vu_sans

/********* ESP32-2432S028R (CYD) *********
 * TFT (ILI9341) : géré par TFT_eSPI (User_Setup)
 * Touch XPT2046 : CLK=25, MISO=39, MOSI=32, CS=33, IRQ=36
 ******************************************/

// --- Touch (bus SPI séparé) ---
#define XPT2046_CS    33
#define XPT2046_IRQ   36
#define XPT2046_CLK   25
#define XPT2046_MISO  39
#define XPT2046_MOSI  32

// --- Résolution du panneau (repère PORTRAIT NATIF) ---
#define PANEL_W 240
#define PANEL_H 320

// --- Résolution logique UI en PAYSAGE ---
#define UI_W 320
#define UI_H 240

// --- Calibration brute (qui marchait chez toi) ---
#define RAW_X_MIN 200
#define RAW_X_MAX 3700
#define RAW_Y_MIN 240
#define RAW_Y_MAX 3800

// Mapping validé : pas de swap, inversions X/Y
#define TOUCH_SWAP_XY   0
#define TOUCH_INVERT_X  1
#define TOUCH_INVERT_Y  1

// --- LVGL draw buffer (~10 % écran) ---
#define DRAW_BUF_SIZE (PANEL_W * PANEL_H / 10 * (LV_COLOR_DEPTH / 8))
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

// ---------- Wi-Fi AP + Web ----------
const char* AP_SSID = "Message_Dispenser";
const char* AP_PASS = "12345678";
WebServer server(80);

// ---------- Données / Listes ----------
enum Mode : uint8_t { MODE_JOKES=0, MODE_POSITIVE=1, MODE_ORACLE=2 };
enum Lang : uint8_t { LANG_FR=0, LANG_EN=1 };

Mode currentMode = MODE_JOKES;
Lang  currentLang = LANG_FR;

// FR
std::vector<String> jokesFR;
std::vector<String> positivesFR;
std::vector<String> oracleFR;
// EN
std::vector<String> jokesEN;
std::vector<String> positivesEN;
std::vector<String> oracleEN;

// Tirage sans répétition (par langue)
std::vector<int> bagJokesFR, bagPosFR;
std::vector<int> bagJokesEN, bagPosEN;

// Objets globaux UI/tactile
SPIClass spiTouch(HSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

static lv_obj_t *text_label, *btnMain, *btnMain_label;
static lv_obj_t *lblMode, *lblWiFi, *lblLang, *swLang;
bool   showingQuestion = true;   // true: bouton=Réponse/Answer ; false: bouton=Suivant/Next
int    currentJoke   = -1;
String currentQ, currentA;       // Q/R persistantes

// Styles (bouton jaune)
static lv_style_t st_btn_def, st_btn_pr;
static bool styles_inited = false;

// ---------- Utils texte ROBUSTES ----------
String normalizeLine(const String& s){
  String t = s;
  t.replace("\r", "");
  t.replace("\xC2\xA0", " "); // NBSP -> espace
  t.trim();
  return t;
}

std::vector<String> splitByEmptyLine(const String& big){
  std::vector<String> out;
  String acc, line;
  auto flushPara = [&](){
    String par = normalizeLine(acc);
    if (par.length()) out.push_back(par);
    acc = "";
  };
  for (uint32_t i = 0; i < big.length(); ++i){
    char c = big[i];
    if (c == '\n'){
      String ln = normalizeLine(line);
      if (ln.length() == 0) { // ligne vide => fin paragraphe
        flushPara();
      } else {
        if (acc.length()) acc += '\n';
        acc += ln;
      }
      line = "";
    } else if (c != '\r') {
      line += c;
    }
  }
  String ln = normalizeLine(line);
  if (ln.length()){
    if (acc.length()) acc += '\n';
    acc += ln;
  }
  flushPara();
  return out;
}

std::vector<String> splitLinesNonEmpty(const String& big){
  std::vector<String> out; String line;
  auto pushLine = [&](){
    String ln = normalizeLine(line);
    if (ln.length()) out.push_back(ln);
    line = "";
  };
  for (uint32_t i=0; i<big.length(); ++i){
    char c = big[i];
    if (c=='\n') pushLine();
    else if (c!='\r') line += c;
  }
  pushLine();
  return out;
}

// ---------- SPIFFS I/O ----------
bool saveText(const char* path, const String& data){
  File f = SPIFFS.open(path, FILE_WRITE);
  if(!f) return false;
  f.print(data);
  f.close();
  return true;
}

String loadText(const char* path){
  File f = SPIFFS.open(path, FILE_READ);
  if(!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

size_t fileSize(const char* path){
  File f = SPIFFS.open(path, FILE_READ);
  if(!f) return 0;
  size_t sz = f.size(); f.close(); return sz;
}

// Petit log de contrôle
void logCounts(const char* tag){
  Serial.printf("[%s]\n", tag);
  Serial.printf("  FR  jokes=%u (%u B)  pos=%u (%u B)  oracle=%u (%u B)\n",
    (unsigned)jokesFR.size(),     (unsigned)fileSize("/jokes_fr.txt"),
    (unsigned)positivesFR.size(), (unsigned)fileSize("/positives_fr.txt"),
    (unsigned)oracleFR.size(),    (unsigned)fileSize("/oracle_fr.txt"));
  Serial.printf("  EN  jokes=%u (%u B)  pos=%u (%u B)  oracle=%u (%u B)\n",
    (unsigned)jokesEN.size(),     (unsigned)fileSize("/jokes_en.txt"),
    (unsigned)positivesEN.size(), (unsigned)fileSize("/positives_en.txt"),
    (unsigned)oracleEN.size(),    (unsigned)fileSize("/oracle_en.txt"));
}

// ---------- Chargement initial ----------
void loadAllFromFS(){
  // FR defaults
  String jfr = loadText("/jokes_fr.txt");
  if (jfr.length()==0){
    jfr =
      "Qu'est ce qui n'est pas un steak?\nUne pastèque.\n\n"
      "Tu connais l'histoire du lit superposé ?\nC'est une histoire à dormir debout.\n";
    saveText("/jokes_fr.txt", jfr);
  }
  jokesFR = splitByEmptyLine(jfr);

  String pfr = loadText("/positives_fr.txt");
  if (pfr.length()==0){
    pfr = "Tu progresses chaque jour.\nSouris, ça aide.\nAujourd’hui est un cadeau.\n";
    saveText("/positives_fr.txt", pfr);
  }
  positivesFR = splitLinesNonEmpty(pfr);

  String ofr = loadText("/oracle_fr.txt");
  if (ofr.length()==0){
    ofr = "Sans aucun doute.\nTrès probable.\nRéessaye plus tard.\n";
    saveText("/oracle_fr.txt", ofr);
  }
  oracleFR = splitLinesNonEmpty(ofr);

  // EN defaults
  String jen = loadText("/jokes_en.txt");
  if (jen.length()==0){
    jen =
      "Why did the scarecrow win an award?\nBecause he was outstanding in his field.\n\n"
      "What do you call fake spaghetti?\nAn impasta.\n";
    saveText("/jokes_en.txt", jen);
  }
  jokesEN = splitByEmptyLine(jen);

  String pen = loadText("/positives_en.txt");
  if (pen.length()==0){
    pen = "You improve every day.\nSmile, it helps.\nToday is a gift.\n";
    saveText("/positives_en.txt", pen);
  }
  positivesEN = splitLinesNonEmpty(pen);

  String oen = loadText("/oracle_en.txt");
  if (oen.length()==0){
    oen = "Without a doubt.\nVery likely.\nAsk again later.\n";
    saveText("/oracle_en.txt", oen);
  }
  oracleEN = splitLinesNonEmpty(oen);

  // Mode / Lang
  String m = loadText("/mode.txt");
  if (m.length()) {
    int im = m.toInt();
    if (im>=0 && im<=2) currentMode = (Mode)im;
  } else {
    saveText("/mode.txt", String((int)currentMode));
  }
  String lang = loadText("/lang.txt");
  if (lang.length()){
    int il = lang.toInt();
    if (il==0 || il==1) currentLang = (Lang)il;
  } else {
    saveText("/lang.txt", String((int)currentLang));
  }

  logCounts("load");
}

// ---------- Tirage sans répétition ----------
template<typename V> void refillBag(std::vector<int>& bag, const V& vec){
  bag.clear(); bag.resize(vec.size());
  for (int i=0;i<(int)vec.size();i++) bag[i]=i;
  for (int i=(int)bag.size()-1;i>0;i--) std::swap(bag[i], bag[random(i+1)]);
}
int drawFromBag(std::vector<int>& bag){
  if (bag.empty()) return -1;
  int id = bag.back(); bag.pop_back(); return id;
}

// ---------- Accès aux listes actives selon langue ----------
const std::vector<String>& activeJokes()     { return (currentLang==LANG_FR) ? jokesFR     : jokesEN; }
const std::vector<String>& activePositives() { return (currentLang==LANG_FR) ? positivesFR : positivesEN; }
const std::vector<String>& activeOracle()    { return (currentLang==LANG_FR) ? oracleFR    : oracleEN; }
std::vector<int>& activeBagJokes()           { return (currentLang==LANG_FR) ? bagJokesFR  : bagJokesEN; }
std::vector<int>& activeBagPos()             { return (currentLang==LANG_FR) ? bagPosFR    : bagPosEN; }

// ---------- Logique Jokes Q/R ----------
void splitJoke(const String& j, String& q, String& a) {
  int cut = j.indexOf('\n');
  if (cut < 0) { q = j; a = (currentLang==LANG_FR) ? "(Pas de reponse)" : "(No answer)"; }
  else { q = j.substring(0, cut); a = j.substring(cut + 1); }
  q = normalizeLine(q); a = normalizeLine(a);
}

// ========== TOUCH → LVGL ==========
void indev_touch_read(lv_indev_t * indev, lv_indev_data_t * data) {
  LV_UNUSED(indev);
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int32_t rx = p.x, ry = p.y;
    if (TOUCH_SWAP_XY) { int32_t t = rx; rx = ry; ry = t; }
    int32_t x = map(rx, RAW_X_MIN, RAW_X_MAX, 0, PANEL_W  - 1); // 240
    int32_t y = map(ry, RAW_Y_MIN, RAW_Y_MAX, 0, PANEL_H - 1); // 320
    if (TOUCH_INVERT_X) x = (PANEL_W  - 1) - x;
    if (TOUCH_INVERT_Y) y = (PANEL_H - 1) - y;
    x = constrain(x, 0, PANEL_W  - 1);
    y = constrain(y, 0, PANEL_H - 1);
    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state   = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ========== Helpers UI ==========
void setBtnText(const char* txt){ lv_label_set_text(btnMain_label, txt); lv_obj_center(btnMain_label); }
void setModeLabel(){
  const char* name =
    (currentMode==MODE_JOKES)    ? ((currentLang==LANG_FR) ? "Mode: Blagues"  : "Mode: Jokes") :
    (currentMode==MODE_POSITIVE) ? ((currentLang==LANG_FR) ? "Mode: Positifs" : "Mode: Positives") :
                                   ((currentLang==LANG_FR) ? "Mode: Oracle"   : "Mode: Oracle");
  lv_label_set_text(lblMode, name);
}
void setWiFiLabel(const IPAddress& ip){
  char buf[40];
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip[0],ip[1],ip[2],ip[3]);
  lv_label_set_text(lblWiFi, buf);
}
void setLangLabel(){
  lv_label_set_text(lblLang, (currentLang==LANG_FR) ? "FR" : "EN");
  // Ajuste texte du bouton principal selon l’état
  if (currentMode==MODE_JOKES){
    setBtnText(showingQuestion ? ((currentLang==LANG_FR)?"Réponse":"Answer")
                               : ((currentLang==LANG_FR)?"Suivant":"Next"));
  } else if (currentMode==MODE_POSITIVE){
    setBtnText((currentLang==LANG_FR)?"Suivant":"Next");
  } else {
    setBtnText((currentLang==LANG_FR)?"Encore":"Again");
  }
}

// --- Affichages selon mode ---
void showNextQuestion(){
  const auto& JV = activeJokes();
  if (JV.empty()){ lv_label_set_text(text_label, (currentLang==LANG_FR)?"Liste blagues vide":"Jokes list empty"); return; }
  auto& bag = activeBagJokes();
  if (bag.empty()) refillBag(bag, JV);
  currentJoke = drawFromBag(bag);
  splitJoke(JV[currentJoke], currentQ, currentA);
  lv_label_set_text(text_label, currentQ.c_str());
  showingQuestion = true;
  setBtnText((currentLang==LANG_FR)?"Réponse":"Answer");
}
void showAnswer(){
  lv_label_set_text(text_label, currentA.c_str());
  showingQuestion = false;
  setBtnText((currentLang==LANG_FR)?"Suivant":"Next");
}
void showNextPositive(){
  const auto& PV = activePositives();
  if (PV.empty()){ lv_label_set_text(text_label, (currentLang==LANG_FR)?"Liste positifs vide":"Positives list empty"); return; }
  auto& bag = activeBagPos();
  if (bag.empty()) refillBag(bag, PV);
  int id = drawFromBag(bag);
  lv_label_set_text(text_label, PV[id].c_str());
  setBtnText((currentLang==LANG_FR)?"Suivant":"Next");
}
void showOracle(){
  const auto& OV = activeOracle();
  if (OV.empty()){ lv_label_set_text(text_label, (currentLang==LANG_FR)?"Liste oracle vide":"Oracle list empty"); return; }
  int id = random(OV.size());
  lv_label_set_text(text_label, OV[id].c_str());
  setBtnText((currentLang==LANG_FR)?"Encore":"Again");
}
void showInitialForMode(){
  setModeLabel();
  switch(currentMode){
    case MODE_JOKES:     showNextQuestion(); break;
    case MODE_POSITIVE:  showNextPositive(); break;
    case MODE_ORACLE:    showOracle();       break;
  }
  setLangLabel();
}

// ========== Callbacks UI ==========
static void on_btnMain(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (currentMode == MODE_JOKES){
    if (showingQuestion) showAnswer();
    else                 showNextQuestion();
  } else if (currentMode == MODE_POSITIVE){
    showNextPositive();
  } else {
    showOracle();
  }
}

static void on_swLang(lv_event_t *e){
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  #if LVGL_VERSION_MAJOR >= 9
    lv_obj_t *sw = (lv_obj_t *) lv_event_get_target(e);
  #else
    lv_obj_t *sw = lv_event_get_target(e);
  #endif
  // Switch ON => EN, OFF => FR
  bool en = lv_obj_has_state(sw, LV_STATE_CHECKED);
  currentLang = en ? LANG_EN : LANG_FR;
  saveText("/lang.txt", String((int)currentLang));
  // Réinitialiser les états contextuels
  showingQuestion = true;
  showInitialForMode();
}

// ========== UI (320×240 paysage) ==========
void create_ui() {
  // Styles (jaune)
  if (!styles_inited) {
    lv_style_init(&st_btn_def);
    lv_style_set_radius(&st_btn_def, 12);
    lv_style_set_bg_color(&st_btn_def, lv_color_hex(0xFFD54F));
    lv_style_set_border_color(&st_btn_def, lv_color_hex(0x333333));
    lv_style_set_border_width(&st_btn_def, 2);
    lv_style_set_text_color(&st_btn_def, lv_color_hex(0x000000));

    lv_style_init(&st_btn_pr);
    lv_style_set_radius(&st_btn_pr, 12);
    lv_style_set_bg_color(&st_btn_pr,  lv_color_hex(0xFBC02D));
    lv_style_set_text_color(&st_btn_pr, lv_color_hex(0x000000));

    styles_inited = true;
  }

  // Bandeau haut : Mode (gauche) + IP (centre) + Lang (droite)
  lblMode = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_font(lblMode, &deja_vu_sans, 0);
  lv_obj_align(lblMode, LV_ALIGN_TOP_LEFT, 8, 6);

  lblWiFi = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_font(lblWiFi, &deja_vu_sans, 0);
  lv_obj_align(lblWiFi, LV_ALIGN_TOP_MID, 0, 6);

  lblLang = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_font(lblLang, &deja_vu_sans, 0);
  lv_obj_align(lblLang, LV_ALIGN_TOP_RIGHT, -58, 6);
  lv_label_set_text(lblLang, "FR");

  swLang = lv_switch_create(lv_scr_act());
  lv_obj_align(swLang, LV_ALIGN_TOP_RIGHT, -8, 4);
  lv_obj_add_event_cb(swLang, on_swLang, LV_EVENT_VALUE_CHANGED, NULL);
  // Position initiale du switch selon currentLang
  if (currentLang==LANG_EN) lv_obj_add_state(swLang, LV_STATE_CHECKED);
  else lv_obj_clear_state(swLang, LV_STATE_CHECKED);

  // Zone texte
  text_label = lv_label_create(lv_scr_act());
  lv_label_set_text(text_label, "Chargement…");
  lv_obj_set_width(text_label, UI_W - 20);
  lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(text_label, &deja_vu_sans, 0);
  lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP);
  lv_obj_align(text_label, LV_ALIGN_CENTER, 0, -18);

  // Bouton unique en bas (jaune)
  int margin = 10;
  int bw = UI_W - margin*2;
  int bh = 54;
  int by = UI_H - bh - margin;

  btnMain = lv_btn_create(lv_scr_act());
  lv_obj_set_size(btnMain, bw, bh);
  lv_obj_align(btnMain, LV_ALIGN_TOP_LEFT, margin, by);
  lv_obj_add_event_cb(btnMain, on_btnMain, LV_EVENT_CLICKED, NULL);
  lv_obj_add_style(btnMain, &st_btn_def, LV_STATE_DEFAULT);
  lv_obj_add_style(btnMain, &st_btn_pr,  LV_STATE_PRESSED);

  btnMain_label = lv_label_create(btnMain);
  lv_obj_set_style_text_font(btnMain_label, &deja_vu_sans, 0);
  lv_label_set_text(btnMain_label, "Réponse");
  lv_obj_center(btnMain_label);
}

// ========== Web helpers: streaming & file echo ==========
void sendChunk(const __FlashStringHelper* s){ server.sendContent(s); }
void sendChunk(const String& s){ server.sendContent(s); }

void sendFileRawIntoTextarea(const char* path){
  File f = SPIFFS.open(path, FILE_READ);
  if(!f){ return; }
  static char buf[1024];
  while (true){
    size_t n = f.readBytes(buf, sizeof(buf));
    if (n == 0) break;
    String chunk(buf, n);
    server.sendContent(chunk);
  }
  f.close();
}

// ========== Web UI (édition FR/EN + choix mode/lang), STREAMING ==========
void handleRoot(){
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");

  sendChunk(F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
              "<title>Gestion des listes FR/EN</title>"
              "<style>body{font-family:system-ui,Arial;margin:16px}textarea{width:100%;min-height:180px;white-space:pre-wrap}"
              ".row{margin:12px 0}.card{border:1px solid #ccc;padding:12px;border-radius:8px;margin-bottom:16px}"
              "button,input[type=submit]{padding:10px 16px;border-radius:8px;border:1px solid #333;background:#ffd54f;cursor:pointer}"
              "label{font-weight:600}select{padding:6px;border-radius:6px}"
              "h2{margin-top:8px}h3{margin:8px 0}</style></head><body>"));

  sendChunk(F("<h2>Choix mode & langue</h2>"));
  sendChunk(F("<form method='POST' action='/set_mode_lang'><div class='row'><label>Mode actif :</label> <select name='active'>"));
  sendChunk(String("<option value='0'") + (currentMode==MODE_JOKES?" selected":"") + ">Blagues / Jokes</option>");
  sendChunk(String("<option value='1'") + (currentMode==MODE_POSITIVE?" selected":"") + ">Positifs / Positives</option>");
  sendChunk(String("<option value='2'") + (currentMode==MODE_ORACLE?" selected":"") + ">Oracle</option>");
  sendChunk(F("</select> &nbsp; <label>Langue :</label> <select name='lang'>"));
  sendChunk(String("<option value='0'") + (currentLang==LANG_FR?" selected":"") + ">FR</option>");
  sendChunk(String("<option value='1'") + (currentLang==LANG_EN?" selected":"") + ">EN</option>");
  sendChunk(F("</select> <input type='submit' value='Appliquer'></div></form>"));

  // --- FR ---
  sendChunk(F("<h2>Listes FR</h2>"));

  sendChunk(F("<div class='card'><h3>Blagues FR (Q puis \\n puis Réponse) — séparées par une ligne vide</h3>"
              "<textarea id='jfr'>"));
  sendFileRawIntoTextarea("/jokes_fr.txt");
  sendChunk(F("</textarea><div><button onclick='save(\"/save_raw?f=jokes_fr.txt\",\"jfr\")'>Enregistrer</button> "
              "<a href=\"/export?f=jokes_fr.txt\" download>Exporter</a></div></div>"));

  sendChunk(F("<div class='card'><h3>Positifs FR (un par ligne)</h3><textarea id='pfr'>"));
  sendFileRawIntoTextarea("/positives_fr.txt");
  sendChunk(F("</textarea><div><button onclick='save(\"/save_raw?f=positives_fr.txt\",\"pfr\")'>Enregistrer</button> "
              "<a href=\"/export?f=positives_fr.txt\" download>Exporter</a></div></div>"));

  sendChunk(F("<div class='card'><h3>Oracle FR (un par ligne)</h3><textarea id='ofr'>"));
  sendFileRawIntoTextarea("/oracle_fr.txt");
  sendChunk(F("</textarea><div><button onclick='save(\"/save_raw?f=oracle_fr.txt\",\"ofr\")'>Enregistrer</button> "
              "<a href=\"/export?f=oracle_fr.txt\" download>Exporter</a></div></div>"));

  // --- EN ---
  sendChunk(F("<h2>Lists EN</h2>"));

  sendChunk(F("<div class='card'><h3>Jokes EN (Q then \\n then Answer) — separated by a blank line</h3>"
              "<textarea id='jen'>"));
  sendFileRawIntoTextarea("/jokes_en.txt");
  sendChunk(F("</textarea><div><button onclick='save(\"/save_raw?f=jokes_en.txt\",\"jen\")'>Save</button> "
              "<a href=\"/export?f=jokes_en.txt\" download>Export</a></div></div>"));

  sendChunk(F("<div class='card'><h3>Positives EN (one per line)</h3><textarea id='pen'>"));
  sendFileRawIntoTextarea("/positives_en.txt");
  sendChunk(F("</textarea><div><button onclick='save(\"/save_raw?f=positives_en.txt\",\"pen\")'>Save</button> "
              "<a href=\"/export?f=positives_en.txt\" download>Export</a></div></div>"));

  sendChunk(F("<div class='card'><h3>Oracle EN (one per line)</h3><textarea id='oen'>"));
  sendFileRawIntoTextarea("/oracle_en.txt");
  sendChunk(F("</textarea><div><button onclick='save(\"/save_raw?f=oracle_en.txt\",\"oen\")'>Save</button> "
              "<a href=\"/export?f=oracle_en.txt\" download>Export</a></div></div>"));

  // JS
  sendChunk(F("<script>"
              "function save(url,id){const v=document.getElementById(id).value;"
              "fetch(url,{method:'POST',headers:{'Content-Type':'text/plain;charset=utf-8'},body:v}).then(_=>location.reload());}"
              "</script>"));

  // footer counts
  sendChunk(F("<p>IP par défaut : <b>http://192.168.4.1</b></p>"));

  sendChunk(F("</body></html>"));
}

// Sauvegarde mode + langue
void handleSetModeLang(){
  if (server.hasArg("active")){
    int m = server.arg("active").toInt();
    if (m>=0 && m<=2){ currentMode = (Mode)m; saveText("/mode.txt", String(m)); }
  }
  if (server.hasArg("lang")){
    int il = server.arg("lang").toInt();
    if (il==0 || il==1){ currentLang = (Lang)il; saveText("/lang.txt", String(il)); }
  }
  showingQuestion = true;
  showInitialForMode();
  server.sendHeader("Location", "/", true);
  server.send(303);
}

// Sauvegarde RAW paramétrée par ?f=...
void handleSaveRaw(){
  if (!server.hasArg("f")) { server.send(400, "text/plain", "Missing f"); return; }
  String fname = server.arg("f");
  String body  = server.arg("plain");

  // Ecrit brut
  if (!saveText(("/"+fname).c_str(), body)) {
    server.send(500, "text/plain", "Write failed");
    return;
  }

  // Recharge le vecteur concerné et vide les bags si besoin
  if (fname == "jokes_fr.txt")     { jokesFR     = splitByEmptyLine(body); bagJokesFR.clear(); showingQuestion = true; }
  else if (fname == "positives_fr.txt"){ positivesFR = splitLinesNonEmpty(body); bagPosFR.clear(); }
  else if (fname == "oracle_fr.txt"){ oracleFR   = splitLinesNonEmpty(body); }
  else if (fname == "jokes_en.txt") { jokesEN    = splitByEmptyLine(body); bagJokesEN.clear(); showingQuestion = true; }
  else if (fname == "positives_en.txt"){ positivesEN = splitLinesNonEmpty(body); bagPosEN.clear(); }
  else if (fname == "oracle_en.txt"){ oracleEN   = splitLinesNonEmpty(body); }

  showInitialForMode();
  logCounts("save_raw");
  server.send(200, "text/plain", "OK");
}

// Export brut via ?f=...
void handleExport(){
  if (!server.hasArg("f")) { server.send(400, "text/plain", "Missing f"); return; }
  String fname = server.arg("f");
  String path = "/" + fname;
  File f = SPIFFS.open(path, FILE_READ);
  if(!f){ server.send(404, "text/plain", "Not found"); return; }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", String("attachment; filename=")+fname);
  server.send(200, "text/plain; charset=utf-8", "");
  static uint8_t buf[1024];
  while(true){
    size_t n = f.read(buf, sizeof(buf));
    if(!n) break;
    server.client().write(buf, n);
  }
  f.close();
}

// ========== Setup / Loop ==========
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("LVGL v%d.%d.%d\n", lv_version_major(), lv_version_minor(), lv_version_patch());

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }
  loadAllFromFS();

  // --- Wi-Fi AP (robuste) ---
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPdisconnect(true);
  delay(100);

  IPAddress apIP(192,168,4,1), apGW(192,168,4,1), apNM(255,255,255,0);
  WiFi.softAPConfig(apIP, apGW, apNM);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 1, false, 4);
  delay(200);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("AP ok=%d  IP=%s  clients=%d\n", apOk, ip.toString().c_str(), WiFi.softAPgetStationNum());

  // Journalisation connexions (core 3.x)
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info){
    switch (e) {
      case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
        auto &st = info.wifi_ap_staconnected;
        Serial.printf("Client connecté: %02X:%02X:%02X:%02X:%02X:%02X  total=%d\n",
                      st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5],
                      WiFi.softAPgetStationNum());
        break;
      }
      case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
        auto &st = info.wifi_ap_stadisconnected;
        Serial.printf("Client parti: %02X:%02X:%02X:%02X:%02X:%02X  total=%d\n",
                      st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5],
                      WiFi.softAPgetStationNum());
        break;
      }
      default: break;
    }
  });

  // Routes Web
  server.on("/",               HTTP_GET,  handleRoot);
  server.on("/ping",           HTTP_GET,  [](){ server.send(200, "text/plain", "pong"); });
  server.on("/set_mode_lang",  HTTP_POST, [](){ handleSetModeLang(); });
  server.on("/save_raw",       HTTP_POST, [](){ handleSaveRaw(); });
  server.on("/export",         HTTP_GET,  [](){ handleExport(); });
  server.begin();

  // LVGL + TFT
  lv_init();
  lv_display_t *disp = lv_tft_espi_create(PANEL_W, PANEL_H, draw_buf, sizeof(draw_buf));
  lv_disp_set_rotation(disp, LV_DISP_ROTATION_90); // paysage

  // Touch sur bus séparé (repère PORTRAIT NATIF)
  spiTouch.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(spiTouch);
  ts.setRotation(0); // bruts en portrait, LVGL gère la rotation

  // Indev tactile LVGL (lié au display)
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, indev_touch_read);
  lv_indev_set_display(indev, disp);

  // UI
  randomSeed(esp_random());
  create_ui();
  setWiFiLabel(ip);
  // position du switch langue selon currentLang
  if (currentLang==LANG_EN) lv_obj_add_state(swLang, LV_STATE_CHECKED);
  else lv_obj_clear_state(swLang, LV_STATE_CHECKED);
  showInitialForMode();
}

void loop() {
  server.handleClient();    // Web
  lv_tick_inc(5);           // LVGL
  lv_timer_handler();
  delay(5);
}
