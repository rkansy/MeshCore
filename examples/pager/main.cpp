#include <Arduino.h>
#include <SPIFFS.h>
#include <RTClib.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/TxtDataHelpers.h>
#include <target.h>
#include "../companion_radio/DataStore.h"

#define PAGER_MAX_INBOX 32
#define PAGER_QUEUE_MAX 10
#define PAGER_TEXT_MAX 300

namespace {
constexpr ColorVal C_BG = 0xEF7D;       // #F4F4F0
constexpr ColorVal C_INK = 0x10A2;      // #161616
constexpr ColorVal C_DIM = 0x5B4B;      // #5C5C58
constexpr ColorVal C_ACCENT = 0x1D5D;   // #1F6FEB
constexpr ColorVal C_OK = 0x3D67;       // #1F7A3D
constexpr ColorVal C_LINE = 0xCE79;     // #C9C9C4
constexpr uint32_t SEND_TIMEOUT_MS = 500;
constexpr uint32_t FLOOD_TIMEOUT_FACTOR = 16;
constexpr uint32_t DIRECT_HOP_FACTOR = 6;
constexpr uint32_t DIRECT_HOP_EXTRA_MS = 250;

struct PagerMessage {
  char sender[32];
  char channel[32];
  char text[PAGER_TEXT_MAX + 1];
  uint32_t timestamp;
  bool unread;
  bool outgoing;
  bool queued;
};

struct PendingMessage {
  uint8_t pubkey[PUB_KEY_SIZE];
  char text[PAGER_TEXT_MAX + 1];
};

struct PagerSettings {
  uint32_t magic;
  uint8_t screen_timeout; // 10s, 30s, 60s, 5m, 10m, never
  uint8_t brightness;
  uint8_t keyboard_light;
  uint8_t current_channel;
};

static const uint32_t SETTINGS_MAGIC = 0x50414731;

class PagerMesh : public BaseChatMesh, public DataStoreHost {
  DataStore* _store;
  NodePrefs _prefs;
  PagerMessage _messages[PAGER_MAX_INBOX];
  uint8_t _message_count = 0;
  uint32_t _message_serial = 0;
  PendingMessage _queue[PAGER_QUEUE_MAX];
  uint8_t _queue_count = 0;
  uint32_t _expected_ack = 0;
  bool _waiting_ack = false;
  uint8_t _current_channel = 0;
  bool _dirty_contacts = false;
  uint32_t _save_contacts_at = 0;
  int _last_rssi = -127;
public:
  PagerMesh(mesh::Radio& radio, StdRNG& rng, mesh::RTCClock& rtc, SimpleMeshTables& tables, DataStore& store)
    : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(24), tables), _store(&store) {
    memset(_messages, 0, sizeof(_messages));
    memset(_queue, 0, sizeof(_queue));
  }

  NodePrefs& prefs() { return _prefs; }
  uint8_t queueSize() const { return _queue_count - (_waiting_ack && _queue_count ? 1 : 0); }
  uint8_t messageCount() const { return _message_count; }
  uint32_t messageSerial() const { return _message_serial; }
  const PagerMessage& message(uint8_t idx) const { return _messages[idx]; }
  uint8_t currentChannel() const { return _current_channel; }
  int lastRSSI() const { return _last_rssi; }
  void setCurrentChannel(uint8_t idx) { _current_channel = idx; }
  void markRead(uint8_t idx) { if (idx < _message_count) _messages[idx].unread = false; }
  void savePreferences() { _store->savePrefs(_prefs); }

  bool start() {
    BaseChatMesh::begin();
    _store->begin();
    _store->loadPrefs(_prefs);
    if (!_prefs.node_name[0]) StrHelper::strncpy(_prefs.node_name, "MESH PAGER", sizeof(_prefs.node_name));
    _prefs.freq = _prefs.freq > 0 ? _prefs.freq : 434.0f;
    _prefs.bw = _prefs.bw > 0 ? _prefs.bw : 250.0f;
    _prefs.sf = _prefs.sf ? _prefs.sf : 10;
    _prefs.cr = _prefs.cr ? _prefs.cr : 5;
    _prefs.tx_power_dbm = _prefs.tx_power_dbm ? _prefs.tx_power_dbm : 17;
    if (!_store->loadMainIdentity(self_id)) {
      self_id = radio_new_identity();
      if (!_store->saveMainIdentity(self_id)) return false;
    }
    _store->loadContacts(this);
    _store->loadChannels(this);
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    radio_driver.setTxPower(_prefs.tx_power_dbm);
    auto advert = createSelfAdvert(_prefs.node_name);
    if (advert) sendFlood(advert, 800);
    return true;
  }

  bool onContactLoaded(const ContactInfo& c) override { return addContact(c); }
  bool getContactForSave(uint32_t idx, ContactInfo& c) override { return getContactByIdx(idx, c); }
  bool onChannelLoaded(uint8_t idx, const ChannelDetails& ch) override { return setChannel(idx, ch); }
  bool getChannelForSave(uint8_t idx, ChannelDetails& ch) override { return getChannel(idx, ch); }

  void saveContacts() { _store->saveContacts(this); _dirty_contacts = false; }
  void saveChannels() { _store->saveChannels(this); }

protected:
  void logRxRaw(float, float rssi, const uint8_t[], int) override { _last_rssi=(int)rssi; }
  float getAirtimeBudgetFactor() const override { return _prefs.airtime_factor > 0 ? _prefs.airtime_factor : 1.0f; }
  int calcRxDelay(float, uint32_t) const override { return 0; }
  bool allowPacketForward(const mesh::Packet*) override { return true; }
  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override { _dirty_contacts = true; _save_contacts_at = millis() + 5000; }
  void onContactPathUpdated(const ContactInfo&) override { _dirty_contacts = true; _save_contacts_at = millis() + 5000; }
  ContactInfo* processAck(const uint8_t* data) override {
    if (_waiting_ack && memcmp(data, &_expected_ack, sizeof(_expected_ack)) == 0) {
      ContactInfo* acknowledged = findPendingContact();
      _waiting_ack = false;
      _expected_ack = 0;
      if (_queue_count) {
        memmove(&_queue[0], &_queue[1], sizeof(_queue[0]) * (_queue_count - 1));
        --_queue_count;
      }
      return acknowledged;
    }
    return nullptr;
  }
  ContactInfo* findPendingContact() {
    if (_queue_count == 0) return nullptr;
    return lookupContactByPubKey(_queue[0].pubkey, PUB_KEY_SIZE);
  }
  void addMessage(const char* sender, const char* channel, const char* text, uint32_t timestamp, bool incoming, bool queued=false) {
    if (_message_count == PAGER_MAX_INBOX) {
      for (uint8_t i=1; i<_message_count; ++i) _messages[i-1] = _messages[i];
      --_message_count;
    }
    ++_message_serial;
    PagerMessage& m = _messages[_message_count++];
    memset(&m, 0, sizeof(m));
    StrHelper::strncpy(m.sender, sender, sizeof(m.sender));
    StrHelper::strncpy(m.channel, channel, sizeof(m.channel));
    StrHelper::strncpy(m.text, text, sizeof(m.text));
    m.timestamp = timestamp;
    m.unread = incoming;
    m.outgoing = !incoming;
    m.queued = queued;
  }
  void onMessageRecv(const ContactInfo& from, mesh::Packet*, uint32_t timestamp, const char* text) override {
    addMessage(from.name, "Direct", text, timestamp, true);
  }
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override { }
  void onCLICommandRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*, char*) override { }
  void onSignedMessageRecv(const ContactInfo& from, mesh::Packet*, uint32_t timestamp, const uint8_t*, const char* text) override {
    addMessage(from.name, "Direct", text, timestamp, true);
  }
  void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet*, uint32_t timestamp, const char* text) override {
    char name[32] = "Channel";
    for (int i=0; i<MAX_GROUP_CHANNELS; ++i) {
      ChannelDetails c;
      if (getChannel(i, c) && memcmp(c.channel.secret, channel.secret, sizeof(channel.secret)) == 0) {
        StrHelper::strncpy(name, c.name, sizeof(name));
        break;
      }
    }
    const char* sep = strchr(text, ':');
    char sender[32] = "?";
    if (sep) {
      size_t n = sep - text;
      if (n >= sizeof(sender)) n = sizeof(sender)-1;
      memcpy(sender, text, n); sender[n] = 0;
      while (*++sep == ' ') { }
      addMessage(sender, name, sep, timestamp, true);
    } else addMessage(name, name, text, timestamp, true);
  }
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override { }
  uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override {
    return min(5000u, SEND_TIMEOUT_MS + FLOOD_TIMEOUT_FACTOR * airtime);
  }
  uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t path_len) const override {
    return min(5000u, SEND_TIMEOUT_MS + ((airtime * DIRECT_HOP_FACTOR + DIRECT_HOP_EXTRA_MS) * ((path_len & 63) + 1)));
  }
  void onSendTimeout() override { _waiting_ack = false; }

public:
  void enqueuePending(const ContactInfo& recipient, const char* text) {
    if (_queue_count == PAGER_QUEUE_MAX) {
      uint8_t drop = _waiting_ack ? 1 : 0; // preserve the in-flight entry
      if (drop >= _queue_count) drop = 0;
      memmove(&_queue[drop], &_queue[drop + 1], sizeof(_queue[0]) * (_queue_count - drop - 1));
      --_queue_count;
    }
    memcpy(_queue[_queue_count].pubkey, recipient.id.pub_key, PUB_KEY_SIZE);
    StrHelper::strncpy(_queue[_queue_count].text, text, sizeof(_queue[_queue_count].text));
    ++_queue_count;
  }
  bool sendDirectMessage(const ContactInfo& recipient, const char* text) {
    if (_waiting_ack) {
      enqueuePending(recipient, text);
      addMessage(recipient.name, "Direct", text, getRTCClock()->getCurrentTime(), false, true);
      return true;
    }
    uint32_t estimate = 0, ack = 0;
    int rc = sendMessage(recipient, getRTCClock()->getCurrentTime(), 0, text, ack, estimate);
    if (rc == MSG_SEND_FAILED) return false;
    _expected_ack = ack;
    _waiting_ack = true;
    enqueuePending(recipient, text);
    addMessage(recipient.name, "Direct", text, getRTCClock()->getCurrentTime(), false);
    return true;
  }
  bool sendChannelMessage(uint8_t idx, const char* text) {
    ChannelDetails c;
    if (!getChannel(idx, c) || !c.name[0]) return false;
    if (!sendGroupMessage(getRTCClock()->getCurrentTime(), c.channel, _prefs.node_name, text, strlen(text))) return false;
    addMessage(_prefs.node_name, c.name, text, getRTCClock()->getCurrentTime(), false);
    return true;
  }
  void queueMessage(const ContactInfo& recipient, const char* text) {
    enqueuePending(recipient, text);
    addMessage(recipient.name, "Direct", text, getRTCClock()->getCurrentTime(), false, true);
  }
  void loop() {
    BaseChatMesh::loop();
    if (_dirty_contacts && (int32_t)(millis() - _save_contacts_at) >= 0) saveContacts();
    if (_queue_count && !_waiting_ack) {
      ContactInfo* found = lookupContactByPubKey(_queue[0].pubkey, PUB_KEY_SIZE);
      if (found) {
        uint32_t ack=0, estimate=0;
        int rc=sendMessage(*found, getRTCClock()->getCurrentTime(), 1, _queue[0].text, ack, estimate);
        if (rc != MSG_SEND_FAILED) { _expected_ack=ack; _waiting_ack=true; }
      }
    }
  }
};

StdRNG pager_rng;
SimpleMeshTables pager_tables;
DataStore pager_store(SPIFFS, rtc_clock);
PagerMesh pager_mesh(radio_driver, pager_rng, rtc_clock, pager_tables, pager_store);
PagerSettings settings = { SETTINGS_MAGIC, 2, 64, 1, 0 };

enum Screen : uint8_t { INBOX, COMPOSE, NODES, CHANNELS, SETTINGS_SCREEN, MESSAGE_VIEW, ADD_CHANNEL };
static Screen screen = INBOX;
static uint8_t selection = 0, message_index = 0;
static uint8_t saved_selection[7] = {0};
static uint8_t compose_focus = 0; // 0 recipient, 1 message
static char draft[PAGER_TEXT_MAX + 1] = {0};
static uint16_t draft_len = 0, caret = 0;
static int16_t recipient_idx = -1;
static uint32_t last_input = 0, last_draw = 0;
static bool screen_off = false, symbol_mode = false;
static bool wake_flash = false;
static uint32_t wake_flash_until = 0;
static uint32_t last_seen_message_serial = 0;
static uint8_t add_focus = 0;
static uint8_t message_scroll = 0;
static bool setting_edit = false;
static char channel_name[21] = {0}, channel_psk[17] = {0};
static char status_message[24] = {0};
static uint32_t status_until = 0;
static int battery_percent = -1;
static uint32_t last_battery = 0;
static MomentaryButton rotary_center(PIN_ROTARY_PRESS, 1000, true, true);
static void saveUISettings() {
  File f=SPIFFS.open("/pager.cfg","w");
  if(f){settings.magic=SETTINGS_MAGIC;f.write((const uint8_t*)&settings,sizeof(settings));f.close();}
}

static void showStatus(const char* msg, uint32_t duration=1000) { StrHelper::strncpy(status_message, msg, sizeof(status_message)); status_until = millis() + duration; }
static void drawText(int x, int y, const char* text, bool small=false, ColorVal color=C_INK) {
  display.setColor(color); display.setTextSize(small ? 1 : 2); display.setCursor(x,y); display.print(text);
}
static void drawStartupFailure(const char* stage) {
  display.startFrame(C_BG);
  drawText(12,75,"STARTUP FAILED",false,C_ACCENT);
  drawText(12,112,stage,true,C_INK);
  drawText(12,142,"Check serial console",true,C_DIM);
  display.endFrame();
}
static void ellipsize(char* dst, size_t size, const char* src, size_t max_chars) {
  if (size == 0) return;
  size_t n = strlen(src);
  if (n <= max_chars) { StrHelper::strncpy(dst, src, size); return; }
  if (max_chars < 4) max_chars = 4;
  if (max_chars >= size) max_chars = size - 1;
  memcpy(dst, src, max_chars - 3); memcpy(dst + max_chars - 3, "...", 4);
}
static void drawStatusBar() {
  display.setColor(C_BG); display.fillRect(0,0,480,24);
  display.setColor(C_LINE); display.fillRect(0,23,480,1);
  char b[24];
  if (battery_percent < 0) snprintf(b,sizeof(b),"--%%"); else snprintf(b,sizeof(b),"[%c%c%c] %d%%", battery_percent>15?'#':'-', battery_percent>45?'#':'-', battery_percent>75?'#':'-', battery_percent);
  drawText(4,5,b,true,battery_percent<=30?C_ACCENT:C_INK);
  int rssi=pager_mesh.lastRSSI();char signal[4]="";if(rssi!=-127)strcpy(signal,rssi>=-70?"^":rssi>=-90?"~":".");drawText(112,5,signal,true,C_DIM);
  char chan[32] = "NO CHANNEL";
  ChannelDetails c;
  if (pager_mesh.getChannel(pager_mesh.currentChannel(), c) && c.name[0]) ellipsize(chan,sizeof(chan),c.name,22);
  drawText(180,5,chan,true,strcmp(chan,"NO CHANNEL")==0?C_ACCENT:C_INK);
  DateTime dt(pager_mesh.getRTCClock()->getCurrentTime());
  char timebuf[20];
  if (pager_mesh.getRTCClock()->getCurrentTime() < 1772323200) strcpy(timebuf,"--:--");
  else snprintf(timebuf,sizeof(timebuf),"%02u:%02u",dt.hour(),dt.minute());
  if (pager_mesh.queueSize()) {
    char q[24]; snprintf(q,sizeof(q),"Q%u %s",pager_mesh.queueSize(),timebuf); drawText(400,5,q,true,C_ACCENT);
  } else drawText(438,5,timebuf,true,C_DIM);
}
static void drawRow(uint8_t row, uint8_t selected, const char* text, const char* meta="", bool unread=false) {
  int y=24+row*32;
  if (row==selected) { display.setColor(C_INK); display.fillRect(0,y,480,32); drawText(12,y+7,text,false,C_BG); drawText(390,y+10,meta,true,C_BG); }
  else { if (unread) { display.setColor(C_ACCENT); display.fillRect(0,y,4,32); } drawText(12,y+7,text,false,unread?C_ACCENT:C_INK); drawText(390,y+10,meta,true,C_DIM); }
  display.setColor(C_LINE); display.fillRect(0,y+31,480,1);
}
static void renderInbox() {
  if (!pager_mesh.messageCount()) { drawText(150,85,"No messages yet"); drawText(150,115,"Press 2 to compose",true,C_DIM); return; }
  uint8_t count=pager_mesh.messageCount();
  uint8_t first=selection>5?selection-5:0;
  for (uint8_t r=0; r<6 && first+r<count; ++r) {
    const PagerMessage& m=pager_mesh.message(count-1-(first+r));
    char line[160], preview[48]; ellipsize(preview,sizeof(preview),m.text,22);
    snprintf(line,sizeof(line),"%-10s %-16s %s",m.channel,m.sender,preview);
    char t[12]; DateTime dt(m.timestamp); snprintf(t,sizeof(t),"%02u:%02u",dt.hour(),dt.minute());
    drawRow(r,selection==first+r?r:255,line,t,m.unread);
  }
}
static uint8_t contactCount() { return min((int)pager_mesh.getNumContacts(), (int)MAX_CONTACTS); }
static void renderNodes() {
  uint8_t n=contactCount();
  if (!n) { drawText(135,90,"No nodes in range yet"); return; }
  uint8_t first=selection>5?selection-5:0;
  for (uint8_t r=0;r<6 && first+r<n;r++) { ContactInfo c; if (!pager_mesh.getContactByIdx(first+r+MAX_ANON_CONTACTS,c)) continue; char line[100], age[12]; uint32_t now=pager_mesh.getRTCClock()->getCurrentTime(); uint32_t secs=now>c.last_advert_timestamp?now-c.last_advert_timestamp:0; if (secs<300) strcpy(age,"now"); else if(secs<3600) snprintf(age,sizeof(age),"%um",(unsigned)(secs/60)); else if(secs<86400) snprintf(age,sizeof(age),"%uh",(unsigned)(secs/3600)); else snprintf(age,sizeof(age),"%ud",(unsigned)(secs/86400)); char hops[8];if(c.out_path_len==OUT_PATH_UNKNOWN)strcpy(hops,"--");else if(c.out_path_len>3)strcpy(hops,"3+");else snprintf(hops,sizeof(hops),"%u",c.out_path_len);snprintf(line,sizeof(line),"%-26s %-8s",c.name,age); drawRow(r,selection==first+r?r:255,line,hops); }
}
static uint8_t channelCount() { uint8_t n=0; for(int i=0;i<MAX_GROUP_CHANNELS;i++){ChannelDetails c;if(pager_mesh.getChannel(i,c)&&c.name[0])n++;} return n; }
static void renderChannels() {
  uint8_t row=0, ordinal=0, first=selection>5?selection-5:0; for(int i=0;i<MAX_GROUP_CHANNELS && row<6;i++){ChannelDetails c;if(!pager_mesh.getChannel(i,c)||!c.name[0])continue;if(ordinal++<first)continue;bool encrypted=false;for(uint8_t k=0;k<sizeof(c.channel.secret);++k)encrypted|=c.channel.secret[k]!=0;char line[80]; snprintf(line,sizeof(line),"%-30s %s",c.name,encrypted?"enc":"open"); drawRow(row,selection==ordinal-1?row:255,line,i==pager_mesh.currentChannel()?"*":"");row++;}
  if (!row) drawText(155,90,"No channels — button to add");
}
static void renderMessage() {
  if (message_index>=pager_mesh.messageCount()) return;
  const PagerMessage& m=pager_mesh.message(message_index);
  drawText(12,28,m.sender); char meta[80]; DateTime dt(m.timestamp); snprintf(meta,sizeof(meta),"%s  %02u:%02u",m.channel,dt.hour(),dt.minute()); drawText(12,49,meta,true,C_DIM);
  const char* p=m.text; int y=78; uint8_t line_no=0; while(*p && y<212){char line[65];size_t n=0;while(p[n]&&p[n]!='\n'&&n<64)n++;memcpy(line,p,n);line[n]=0;if(line_no++>=message_scroll)drawText(12,y,line);p+=n;if(*p=='\n')p++;y+=28;}
}
static void renderCompose() {
  char to[48]="pick recipient with rotary";
  if(recipient_idx==-2){ChannelDetails ch;if(pager_mesh.getChannel(pager_mesh.currentChannel(),ch)&&ch.name[0])snprintf(to,sizeof(to),"#%s",ch.name);else strcpy(to,"NO CHANNEL");}
  else if(recipient_idx>=MAX_ANON_CONTACTS){ContactInfo c;if(pager_mesh.getContactByIdx(recipient_idx,c))StrHelper::strncpy(to,c.name,sizeof(to));}
  drawText(12,27,"TO:",true,compose_focus==0?C_ACCENT:C_DIM); drawText(52,27,to,false,compose_focus==0?C_INK:C_DIM);
  if(compose_focus==0){uint8_t n=contactCount(), first=selection>5?selection-5:0;for(uint8_t row=0;row<6&&first+row<n;row++){ContactInfo c;if(pager_mesh.getContactByIdx(first+row+MAX_ANON_CONTACTS,c))drawRow(row,selection==first+row?row:255,c.name);}if(n<=first+5){ChannelDetails ch;char broadcast[40];if(pager_mesh.getChannel(pager_mesh.currentChannel(),ch)&&ch.name[0])snprintf(broadcast,sizeof(broadcast),"#%s (broadcast)",ch.name);else strcpy(broadcast,"NO CHANNEL — press 4");drawRow(n-first,selection==n?n-first:255,broadcast);}}
  else {
    const char* p=draft; int y=55; int line=0; while(*p&&line<6){char text[65];size_t n=0;while(p[n]&&n<64)n++;memcpy(text,p,n);text[n]=0;drawText(12,y,text);p+=n;y+=27;line++;}
    if (millis()%1000<500 && draft_len< PAGER_TEXT_MAX) { int x=12+(caret%64)*7; int y=55+(caret/64)*27+14; display.setColor(C_INK); display.fillRect(x,y,7,2); }
  }
  char count[32];snprintf(count,sizeof(count),"CHARS %u/300%s",draft_len,draft_len>280?" !":"");drawText(12,202,count,true,draft_len>280?C_ACCENT:C_DIM);
  if(symbol_mode)drawText(420,202,"SYM",true,C_ACCENT);
  if(status_message[0]&&(int32_t)(status_until-millis())>0)drawText(180,202,status_message,true,C_OK);
}
static void renderSettings() {
  char values[9][32];
  snprintf(values[0],32,"%s",pager_mesh.prefs().node_name);snprintf(values[1],32,"%.1f MHz",pager_mesh.prefs().freq);snprintf(values[2],32,"SF%u",pager_mesh.prefs().sf);snprintf(values[3],32,"%.0f kHz",pager_mesh.prefs().bw);snprintf(values[4],32,"4/%u",pager_mesh.prefs().cr);snprintf(values[5],32,"%d dBm",pager_mesh.prefs().tx_power_dbm);
  const char* timeout_names[]={"10s","30s","1min","5min","10min","always"};snprintf(values[6],32,"%s",timeout_names[settings.screen_timeout]);snprintf(values[7],32,"%u%%",settings.brightness);snprintf(values[8],32,"%s",settings.keyboard_light?"on":"off");
  const char* labels[]={"NODE NAME","FREQ","SF","BW","CR","TX PWR","SCREEN OFF","BRIGHTNESS","KB LIGHT"};
  uint8_t first=selection>5?selection-5:0;
  for(uint8_t r=0;r<6;r++){uint8_t idx=first+r;if(idx>=9)break;char line[64];snprintf(line,sizeof(line),"%-17s %s",labels[idx],values[idx]);drawRow(r,idx==selection?r:255,line);}
}
static void renderAddChannel(){drawText(12,38,"NAME:",true,add_focus==0?C_ACCENT:C_DIM);drawText(78,32,channel_name);drawText(12,90,"PSK:",true,add_focus==1?C_ACCENT:C_DIM);drawText(78,84,channel_psk);drawText(12,156,"Center: Join   Button: Cancel",true,C_DIM);}
static void render() {
  if(screen_off) return;
  display.startFrame(C_BG);
  if(screen==INBOX){drawStatusBar();renderInbox();}
  else if(screen==MESSAGE_VIEW){drawStatusBar();renderMessage();}
  else if(screen==COMPOSE){drawStatusBar();renderCompose();}
  else if(screen==NODES){drawStatusBar();renderNodes();}
  else if(screen==CHANNELS){drawStatusBar();renderChannels();}
  else if(screen==SETTINGS_SCREEN){drawStatusBar();renderSettings();}
  else if(screen==ADD_CHANNEL){drawStatusBar();renderAddChannel();}
  display.endFrame();
}
static void wake() { last_input=millis(); if(screen_off){screen_off=false;display.turnOn();keyboard.setBacklight(settings.keyboard_light?settings.brightness*255/100:0);render();} }
static void changeScreen(Screen s){saved_selection[screen]=selection;screen=s;selection=saved_selection[s];symbol_mode=false;setting_edit=false;if(s==COMPOSE){compose_focus=(recipient_idx>=0||recipient_idx==-2)?1:0;caret=draft_len;}render();}
static void saveDraftAndHome(){ changeScreen(INBOX); }
static void beginCompose(int contact=-1){saved_selection[screen]=selection;screen=COMPOSE;compose_focus=contact>=0?1:0;if(contact>=0)recipient_idx=contact;else if(draft_len==0)recipient_idx=-1;selection=recipient_idx>=MAX_ANON_CONTACTS?recipient_idx-MAX_ANON_CONTACTS:0;render();}
static void sendChannel();
static bool sendCurrent() {
  if(recipient_idx==-2){if(!draft_len){showStatus("Empty message",1000);return false;}if(draft_len>MAX_TEXT_LEN-strlen(pager_mesh.prefs().node_name)-2){showStatus("Mesh text limit",1500);return false;}sendChannel();return true;}
  if(recipient_idx<0||!draft_len){showStatus(recipient_idx<0?"Select recipient":"Empty message",1000);return false;}
  if(draft_len>MAX_TEXT_LEN){showStatus("Mesh text limit",1500);return false;}
  ContactInfo c;if(!pager_mesh.getContactByIdx(recipient_idx,c))return false;
  if(!pager_mesh.sendDirectMessage(c,draft)){pager_mesh.queueMessage(c,draft);}
  draft[0]=0;draft_len=caret=0;recipient_idx=-1;showStatus("Sending...",5000);changeScreen(INBOX);return true;
}
static void sendChannel(){if(!draft_len||!pager_mesh.sendChannelMessage(pager_mesh.currentChannel(),draft)){showStatus("Cannot send",1200);return;} draft[0]=0;draft_len=caret=0;changeScreen(INBOX);}
static void addChannel() {
  if(!channel_name[0]){showStatus("Name required");return;}
  ChannelDetails c;memset(&c,0,sizeof(c));StrHelper::strncpy(c.name,channel_name,sizeof(c.name));
  size_t len=strlen(channel_psk);if(len>16||(len&1)){showStatus("PSK must be even hex");return;}
  for(size_t i=0;i<len;i+=2){char byte[3]={channel_psk[i],channel_psk[i+1],0};char* end=nullptr;long v=strtol(byte,&end,16);if(!end||*end){showStatus("Invalid PSK");return;}c.channel.secret[i/2]=(uint8_t)v;}
  mesh::Utils::sha256(c.channel.hash,sizeof(c.channel.hash),c.channel.secret,16);
  int slot=-1;for(int i=0;i<MAX_GROUP_CHANNELS;i++){ChannelDetails old;if(!pager_mesh.getChannel(i,old)||!old.name[0]){slot=i;break;}}
  if(slot<0||!pager_mesh.setChannel(slot,c)){showStatus("Channel list full");return;}
  pager_mesh.setCurrentChannel(slot);pager_mesh.saveChannels();settings.current_channel=slot;saveUISettings();channel_name[0]=channel_psk[0]=0;changeScreen(CHANNELS);
}
static void key(char k) {
  wake();
  if(screen==MESSAGE_VIEW&&((k>='a'&&k<='z')||(k>='A'&&k<='Z'))){const PagerMessage& m=pager_mesh.message(message_index);for(uint8_t i=0;i<contactCount();i++){ContactInfo c;if(pager_mesh.getContactByIdx(i+MAX_ANON_CONTACTS,c)&&strcmp(c.name,m.sender)==0){beginCompose(i+MAX_ANON_CONTACTS);return;}}}
  bool typing = (screen==COMPOSE && compose_focus==1) ||
                (screen==SETTINGS_SCREEN && setting_edit && selection==0) || screen==ADD_CHANNEL;
  if(k>='1'&&k<='5'&&!typing){
    if(screen==MESSAGE_VIEW&&k=='1'){changeScreen(INBOX);return;}
    changeScreen((Screen)(k-'1'));return;
  }
  if(screen==COMPOSE&&compose_focus==1&&k=='1'){saveDraftAndHome();return;}
  if(screen==COMPOSE&&compose_focus==1){
    if(k==TCA8418Keyboard::SELECT){sendCurrent();return;}
    if(k==TCA8418Keyboard::BSP){if(caret){memmove(&draft[caret-1],&draft[caret],draft_len-caret+1);--caret;--draft_len;}render();return;}
    if(k==TCA8418Keyboard::TAB){symbol_mode=!symbol_mode;render();return;}
    if(k>=32&&k<127&&draft_len<PAGER_TEXT_MAX){if(k>='A'&&k<='Z')k+=32;if(!draft_len&&k>='a'&&k<='z')k-=32;memmove(&draft[caret+1],&draft[caret],draft_len-caret+1);draft[caret++]=k;++draft_len;render();}return;
  }
  if(screen==SETTINGS_SCREEN&&setting_edit&&selection==0){char* name=pager_mesh.prefs().node_name;size_t n=strlen(name);if(k==TCA8418Keyboard::BSP){if(n)name[n-1]=0;}else if(k>=32&&k<127&&n<20){name[n]=k;name[n+1]=0;}pager_mesh.savePreferences();render();return;}
  if(screen==ADD_CHANNEL){char* field=add_focus?channel_psk:channel_name;size_t cap=add_focus?16:20;size_t n=strlen(field);if(k==TCA8418Keyboard::BSP){if(n)field[n-1]=0;}else if(k>=32&&k<127&&n<cap)field[n]=k;render();}
}
static void rotate(int direction) {
  wake();
  if(screen==MESSAGE_VIEW){if(pager_mesh.messageCount()){const PagerMessage& m=pager_mesh.message(message_index);if(direction>0&&message_scroll<strlen(m.text)/64)++message_scroll;else if(direction<0&&message_scroll)--message_scroll;render();}return;}
  if(screen==SETTINGS_SCREEN&&setting_edit){NodePrefs& p=pager_mesh.prefs();switch(selection){case 0:break;case 1:p.freq+=direction*0.5f;if(p.freq<400)p.freq=470;if(p.freq>470)p.freq=400;break;case 2:p.sf=(p.sf<=7&&direction<0)?12:(p.sf>=12&&direction>0)?7:p.sf+direction;break;case 3:p.bw=direction>0?(p.bw==500?780:p.bw==780?1031:500):(p.bw==500?1031:p.bw==1031?780:500);break;case 4:p.cr=direction>0?(p.cr>=8?5:p.cr+1):(p.cr<=5?8:p.cr-1);break;case 5:p.tx_power_dbm=constrain(p.tx_power_dbm+direction,0,17);break;case 6:settings.screen_timeout=(settings.screen_timeout+6+direction)%6;saveUISettings();break;case 7:settings.brightness=constrain(settings.brightness+direction*10,10,100);display.setBrightness(settings.brightness*255/100);saveUISettings();break;case 8:settings.keyboard_light=!settings.keyboard_light;keyboard.setBacklight(settings.keyboard_light?settings.brightness*255/100:0);saveUISettings();break;}pager_mesh.savePreferences();render();return;}
  if(screen==COMPOSE&&compose_focus==1){if(caret<draft_len||direction>0)caret=(caret+draft_len+direction)%max(1,(int)draft_len+1);render();return;}
  if(screen==ADD_CHANNEL){add_focus=direction>0?1:0;render();return;}
  uint8_t maxsel=0;
  if(screen==INBOX)maxsel=max(0,(int)pager_mesh.messageCount()-1);
  else if(screen==NODES)maxsel=max(0,(int)contactCount()-1);
  else if(screen==CHANNELS)maxsel=max(0,(int)channelCount()-1);
  else if(screen==SETTINGS_SCREEN)maxsel=8;
  else if(screen==COMPOSE)maxsel=contactCount();
  selection=direction>0?(selection>=maxsel?0:selection+1):(selection==0?maxsel:selection-1);
  render();
}
static void select() {
  wake();
  if(screen==INBOX&&pager_mesh.messageCount()){saved_selection[INBOX]=selection;message_index=pager_mesh.messageCount()-1-min(selection,(uint8_t)(pager_mesh.messageCount()-1));pager_mesh.markRead(message_index);screen=MESSAGE_VIEW;render();}
  else if(screen==MESSAGE_VIEW){}
  else if(screen==COMPOSE){if(compose_focus==0){if(selection<contactCount()){recipient_idx=selection+MAX_ANON_CONTACTS;compose_focus=1;caret=draft_len;}else{ChannelDetails ch;if(pager_mesh.getChannel(pager_mesh.currentChannel(),ch)&&ch.name[0]){recipient_idx=-2;compose_focus=1;caret=draft_len;}else showStatus("NO CHANNEL",1000);}}else sendCurrent();render();}
  else if(screen==NODES&&contactCount()){beginCompose(selection+MAX_ANON_CONTACTS);}
  else if(screen==CHANNELS){int found=-1;uint8_t irow=0;for(int i=0;i<MAX_GROUP_CHANNELS;i++){ChannelDetails c;if(pager_mesh.getChannel(i,c)&&c.name[0]){if(irow++==selection){found=i;break;}}}if(found>=0){pager_mesh.setCurrentChannel(found);pager_mesh.saveChannels();settings.current_channel=found;saveUISettings();render();}}
  else if(screen==SETTINGS_SCREEN){if(setting_edit){setting_edit=false;pager_mesh.savePreferences();saveUISettings();}else setting_edit=true;render();}
  else if(screen==ADD_CHANNEL){if(add_focus==0)add_focus=1;else addChannel();render();}
}
static void back() {
  wake();
  if(screen==COMPOSE){saveDraftAndHome();return;}
  if(screen==ADD_CHANNEL){changeScreen(CHANNELS);return;}
  if(screen==CHANNELS){saved_selection[CHANNELS]=selection;screen=ADD_CHANNEL;add_focus=0;render();return;}
  if(screen==MESSAGE_VIEW){changeScreen(INBOX);return;}
  changeScreen(INBOX);
}
} // namespace

void setup() {
  Serial.begin(115200);
#ifdef PAGER_DEBUG
  uint32_t serial_wait_started=millis();
  while(!Serial && millis()-serial_wait_started<8000) delay(10);
#endif
  Serial.println("[pager] board init");
  board.begin();
  if(!display.begin()) { Serial.println("[pager] display init failed"); return; }
  keyboard.begin();rotary.begin();user_btn.begin();rotary_center.begin();
  settings.brightness=64;settings.keyboard_light=1;
  if(!SPIFFS.begin(true)) {
    Serial.println("[pager] SPIFFS mount failed");
    drawStartupFailure("SPIFFS MOUNT FAILED");
    return;
  }
  File config=SPIFFS.open("/pager.cfg","r"); if(config){PagerSettings loaded;if(config.read((uint8_t*)&loaded,sizeof(loaded))==sizeof(loaded)&&loaded.magic==SETTINGS_MAGIC)settings=loaded;config.close();}
  if(settings.screen_timeout>5)settings.screen_timeout=2;
  if(settings.brightness<10||settings.brightness>100)settings.brightness=64;
#ifdef PAGER_DEBUG
  settings.screen_timeout=5; // keep the display awake while collecting boot diagnostics
#endif
  display.setBrightness(settings.brightness*255/100);keyboard.setBacklight(settings.keyboard_light?settings.brightness*255/100:0);
  display.startFrame(C_BG);drawText(170,86,"MESH PAGER");drawText(210,112,"v0.1.0",true,C_DIM);display.endFrame();
  Serial.println("[pager] radio init");
  if(!radio_init()) {
    Serial.println("[pager] radio init failed");
    drawStartupFailure("RADIO INIT FAILED");
    return;
  }
  Serial.println("[pager] radio init ok; starting mesh");
  pager_rng.begin(radio_driver.getRngSeed());
  if(!pager_mesh.start()) {
    Serial.println("[pager] mesh/storage init failed");
    drawStartupFailure("MESH START FAILED");
    return;
  }
  Serial.println("[pager] mesh start ok");
  pager_mesh.setCurrentChannel(settings.current_channel);
  ChannelDetails selected_channel;
  if(!pager_mesh.getChannel(settings.current_channel,selected_channel)||!selected_channel.name[0]){
    for(uint8_t i=0;i<MAX_GROUP_CHANNELS;i++){if(pager_mesh.getChannel(i,selected_channel)&&selected_channel.name[0]){pager_mesh.setCurrentChannel(i);settings.current_channel=i;saveUISettings();break;}}
  }
  last_seen_message_serial=pager_mesh.messageSerial();
  last_input=millis();render();
}

void loop() {
  pager_mesh.loop();rtc_clock.tick();
  keyboard.update();while(keyboard.hasKey())key(keyboard.readKey());
  RotaryInputEvent ev=rotary.poll();if(ev==RotaryInputEvent::Next)rotate(1);else if(ev==RotaryInputEvent::Prev)rotate(-1);
  if(user_btn.check()==BUTTON_EVENT_CLICK)back();
  if(rotary_center.check()==BUTTON_EVENT_CLICK)select();
  if(millis()-last_battery>1000){last_battery=millis();int p=board.getBattPercent();if(p!=battery_percent){battery_percent=p;if(!screen_off)render();}}
  if(pager_mesh.messageSerial()!=last_seen_message_serial){
    if(screen_off){screen=INBOX;screen_off=false;wake_flash=true;wake_flash_until=millis()+1500;display.turnOn();keyboard.setBacklight(settings.keyboard_light?settings.brightness*255/100:0);render();}
    last_seen_message_serial=pager_mesh.messageSerial();
  }
  if(wake_flash&&(int32_t)(millis()-wake_flash_until)>=0){wake_flash=false;screen_off=true;display.turnOff();keyboard.setBacklight(false);}
  uint32_t timeout_ms[]={10000,30000,60000,300000,600000,0};
  if(!screen_off&&timeout_ms[settings.screen_timeout]&&millis()-last_input>timeout_ms[settings.screen_timeout]){screen_off=true;display.turnOff();keyboard.setBacklight(false);}
  if(!screen_off&&millis()-last_draw>500){last_draw=millis();if(status_message[0]&&(int32_t)(millis()-status_until)>=0)status_message[0]=0;}
  delay(5);
}
