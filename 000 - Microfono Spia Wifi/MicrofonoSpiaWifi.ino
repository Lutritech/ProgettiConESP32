/* https://www.lutritech.it/microfono-spia-wifi/

  Microfono Spia Wifi con Esp32-S2 Mini e INMP441
  
  https://www.youtube.com/@Lutritech
  https://www.facebook.com/lutritech.it
  https://www.instagram.com/Lutritech
  https://github.com/Lutritech

*/
#include <HTTPClient.h>  //per l'invio delle notifiche push
#include <WiFi.h>
#include <WiFiManager.h>
#include <driver/i2s.h>
#include <Arduino.h>

//====PINS PER ESP32-S2 MINI=========
#define I2S_WS 16   
#define I2S_SD 9    
#define I2S_SCK 18  
//===================================

#define I2S_PORT I2S_NUM_0

//---- Sampling ------------
#define SAMPLE_RATE 44100  // Sample rate of the audio
#define SAMPLE_BITS 32     // Bits per sample of the audio

#define DMA_BUF_COUNT 2
#define DMA_BUF_LEN 1024



// ----Audio WAV configuration ------------
const int sampleRate = SAMPLE_RATE;     // Sample rate of the audio
const int bitsPerSample = SAMPLE_BITS;  // Bits per sample of the audio
const int numChannels = 1;              // Number of audio channels (1 for mono, 2 for stereo)
const int bufferSize = DMA_BUF_LEN;     // Buffer size for I2S data transfer


struct WAVHeader {
  char chunkId[4];         // 4 bytes
  uint32_t chunkSize;      // 4 bytes
  char format[4];          // 4 bytes
  char subchunk1Id[4];     // 4 bytes
  uint32_t subchunk1Size;  // 4 bytes
  uint16_t audioFormat;    // 2 bytes
  uint16_t numChannels;    // 2 bytes
  uint32_t sampleRate;     // 4 bytes
  uint32_t byteRate;       // 4 bytes
  uint16_t blockAlign;     // 2 bytes
  uint16_t bitsPerSample;  // 2 bytes
  char subchunk2Id[4];     // 4 bytes
  uint32_t subchunk2Size;  // 4 bytes
};
//-----------------------------------------

WebServer Audioserver(81);
void audio_http_stream();
String ip = "x.x.x.x";  //default ip



void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  WiFiManager wifiManager;
  //wifiManager.resetSettings();  //cancella le reti wifi memorizzate
  wifiManager.setAPCallback(configModeCallback);
  if (!wifiManager.autoConnect("MicrofonoWifiSpia")) {
    Serial.println("Connessione fallita - Riavviare..");
    delay(3000);
    //resetta e prova di nuovo
    ESP.restart();
    delay(5000);
  }
  ip = WiFi.localIP().toString();

  audio_http_stream();
  String messaggio = "http://" + ip + ":81/audio";
  Serial.println(messaggio);
  // Invia la notifica push attraverso l'app di pushover
  sendPushoverNotification("Url da Visitare: ", messaggio.c_str());
}

void loop() {
  Audioserver.handleClient();
}

//Se non si collega alle rete wifi memorizzate si attiva la modalità di configurazione
void configModeCallback(WiFiManager* myWiFiManager) {
  Serial.println("CONFIG MODE");
  Serial.println("VISIT");
  Serial.println("http://" + ip);
  delay(2000);

  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void initializeWAVHeader(WAVHeader& header, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t numChannels) {

  strncpy(header.chunkId, "RIFF", 4);
  strncpy(header.format, "WAVE", 4);
  strncpy(header.subchunk1Id, "fmt ", 4);
  strncpy(header.subchunk2Id, "data", 4);

  header.chunkSize = 0;       // Placeholder for Chunk Size (to be updated later)
  header.subchunk1Size = 16;  // PCM format size (constant for uncompressed audio)
  header.audioFormat = 1;     // PCM audio format (constant for uncompressed audio)
  header.numChannels = numChannels;
  header.sampleRate = sampleRate;
  header.bitsPerSample = bitsPerSample;
  header.byteRate = (sampleRate * bitsPerSample * numChannels) / 8;
  header.blockAlign = (bitsPerSample * numChannels) / 8;
  header.subchunk2Size = 0;  // Placeholder for data size (to be updated later)
}

//======== Medoto che inizializza il bus i2s secondo la configurazione========================
void mic_i2s_init() {

  i2s_config_t i2sConfig = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = sampleRate,
    .bits_per_sample = i2s_bits_per_sample_t(SAMPLE_BITS),
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,  // default interrupt priority
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = true
  };
  i2s_driver_install(I2S_PORT, &i2sConfig, 0, NULL);

  i2s_pin_config_t pinConfig = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };
  i2s_set_pin(I2S_PORT, &pinConfig);
}
//=============================================================================================

//--------------------------Creazione AudioStream-------------------------------------
void handleAudioStream() {
  mic_i2s_init();
  WAVHeader wavHeader;
  initializeWAVHeader(wavHeader, sampleRate, bitsPerSample, numChannels);
  WiFiClient Audioclient = Audioserver.client();

  // Send the 200 OK response with the headers
  Audioclient.print("HTTP/1.1 200 OK\r\n");
  Audioclient.print("Content-Type: audio/wav\r\n");
  Audioclient.print("Access-Control-Allow-Origin: *\r\n");
  Audioclient.print("\r\n");

  // Send the initial part of the WAV header
  Audioclient.write(reinterpret_cast<const uint8_t*>(&wavHeader), sizeof(wavHeader));

  uint8_t buffer[bufferSize];
  size_t bytesRead = 0;
  //uint32_t totalDataSize = 0; // Total size of audio data sent

  while (true) {
    if (!Audioclient.connected()) {
      Serial.println("Audioclient disconnected");
      break;
    }
    // Read audio data from I2S DMA
    i2s_read(I2S_PORT, buffer, bufferSize, &bytesRead, portMAX_DELAY);

    // Send audio data
    if (bytesRead > 0) {
      Audioclient.write(buffer, bytesRead);
    }
  }
}
//----------------------------------------------------------------------------------

void audio_http_stream() {
  Audioserver.on("/audio", HTTP_GET, handleAudioStream);
  Audioserver.begin();
}


//==========Metodo che Invia la notifica push con url da visitare=============================================
void sendPushoverNotification(const char* title, const char* message) {
  // Inserisci il TUO token API di Pushover e il TUO utente
const char* pushover_token = "abbrbd76ngbqs1c9ejf1rzkcu1faqc";
const char* pushover_user = "hgfgjhgwdjhgfweity92445";
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("https://api.pushover.net/1/messages.json");

    // Aggiungi l'header per indicare che stiamo inviando dati di tipo application/x-www-form-urlencoded
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    // Costruisci il messaggio HTML
    String body = "token=" + String(pushover_token) + "&user=" + String(pushover_user) + "&title=" + String(title) + "&message=" + String(message) + "&html=1";

    // Invia la richiesta HTTP POST
    int httpResponseCode = http.POST(body);

    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.println(httpResponseCode);
      Serial.println(response);
    } else {
      Serial.print("Errore nella richiesta: ");
      Serial.println(httpResponseCode);
    }

    http.end();
  } else {
    Serial.println("WiFi non connesso");
  }
}
//============================================================================================================