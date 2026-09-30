#include <FS.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <WebServer.h>

const char *ssid = "MANHU";
const char *password = "123456780";

WebServer server(80);
File uploadFile;
String currentFirmware = ""; 

#define BLOCK_SIZE 256
#define UART_TIMEOUT_MS 5000
#define ACK_BYTE 0x06       
#define STM32_RESET_PIN 4

uint32_t Bootloader_CalcChecksum(uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

bool waitForResponse(const char* expected, uint32_t timeout = 1000)
{
    uint32_t start = millis();
    String response = "";
    while (millis() - start < timeout) {
        if (Serial2.available()) {
            char c = Serial2.read();
            response += c;
            if (response.endsWith(expected)) return true;
        }
    }
    Serial.printf("Expected '%s' but got '%s'\n", expected, response.c_str());
    return false;
}

void handleUpload()
{
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        currentFirmware = "/" + String(upload.filename); 
        uploadFile = SPIFFS.open(currentFirmware, FILE_WRITE);
        if (!uploadFile) {
            Serial.printf("Cant open file %s\n", currentFirmware.c_str());
            return;
        }
        Serial.println("Upload...");
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            Serial.println("Upload done.");
        }
    }
}

bool sendFirmwareToSTM32()
{
    if (currentFirmware == "") {
        Serial.println("No firmware uploaded yet.");
        return false;
    }

    File file = SPIFFS.open(currentFirmware, FILE_READ);
    if (!file) return false;
    uint32_t fw_size = file.size();
    if (fw_size == 0) { file.close(); return false; }

    // Tính checksum toàn file
    uint8_t data[BLOCK_SIZE];
    uint32_t fw_sum = 0;
    uint32_t remaining = fw_size;
    while (remaining > 0) {
        int len = file.read(data, min((uint32_t)BLOCK_SIZE, remaining));
        fw_sum += Bootloader_CalcChecksum(data, len);
        remaining -= len;
    }
    file.seek(0);

    while(Serial2.available()) Serial2.read();

    uint32_t start_time = millis(); 

    Serial.println("Resetting STM32...");
    digitalWrite(STM32_RESET_PIN, LOW); delay(100);
    digitalWrite(STM32_RESET_PIN, HIGH); delay(200);
    
    if (!waitForResponse("BL_READY\r\n", 6000)) { file.close(); return false; }
    delay(10); 

    // Gửi Header
    uint8_t header[8] = {
        (uint8_t)(fw_size & 0xFF), (uint8_t)((fw_size >> 8) & 0xFF), (uint8_t)((fw_size >> 16) & 0xFF), (uint8_t)((fw_size >> 24) & 0xFF),
        (uint8_t)(fw_sum & 0xFF), (uint8_t)((fw_sum >> 8) & 0xFF), (uint8_t)((fw_sum >> 16) & 0xFF), (uint8_t)((fw_sum >> 24) & 0xFF)
    };
    Serial2.write(header, 8);
    if (!waitForResponse("READY\r\n", 5000)) { file.close(); return false; }

    remaining = fw_size;
    uint8_t seq = 0;     
    int blockCount = 0;
    while(Serial2.available()) Serial2.read();

    while (remaining > 0) {
        int len = file.read(data, min((uint32_t)BLOCK_SIZE, remaining));
        
        // Tính CRC cho gói DATA 
        uint8_t packet_crc = 0;
        for(int i = 0; i < len; i++) packet_crc ^= data[i];

        // === TEST KỊCH BẢN 2 ===
        //if (blockCount == 2) {  
        //    packet_crc = packet_crc + 1; // Cố tình cộng sai giá trị CRC
        //    Serial.println("\n>>> [TEST KỊCH BẢN 2] CO TINH LAM SAI CRC O BLOCK 3 !!! <<<\n");
        //}

        int retry = 5; 
        bool sentSuccess = false;

        while (retry > 0) {
            // 1. Gửi gói tin: [SEQ] + [DATA] + [CRC]
            Serial2.write(&seq, 1);
            Serial2.write(data, len);
            Serial2.write(&packet_crc, 1);
            
            // 2. Chờ phản hồi ACK (0x06) hoặc NACK (0x15)
            uint32_t start = millis();
            bool got_response = false;
            
            while (millis() - start < 2000) { // Chờ tối đa 2s
                if (Serial2.available()) {
                    uint8_t c = Serial2.read();
                    if (c == 0x06) { // Nhận được ACK
                        sentSuccess = true;
                        got_response = true;
                        break;
                    } else if (c == 0x15) { // Nhận được NACK
                        Serial.printf("Block %d (SEQ: %d) NACK! Retrying...\n", blockCount+1, seq);
                        got_response = true;
                        break; // Thoát vòng chờ, lặp lại vòng retry
                    }
                }
            }

            if (sentSuccess) break; // Thành công, thoát vòng retry
            
            // Nếu rớt mạng hoặc nhận NACK
            if (!got_response) Serial.printf("Block %d (SEQ: %d) Timeout! Retrying...\n", blockCount+1, seq);
            retry--;
            delay(100); // Chờ một chút cho STM32 ổn định
            while(Serial2.available()) Serial2.read(); // Xóa rác RX
        }

        if (!sentSuccess) {
            Serial.println("Failed to send block after max retries. Abort!");
            file.close(); return false;
        }

        remaining -= len;
        blockCount++;
        seq++; // Tăng số thứ tự cho gói tiếp theo
        Serial.printf("Sent block %d - ACK\n", blockCount);
    }

    file.close();
    Serial.println("Finished sending firmware.");
    
    if(waitForResponse("FW_OK\r\n", 2000)) {
       Serial.println("STM32 Booting app...");

       // === KẾT QUẢ THỜI GIAN NẠP ===
       uint32_t end_time = millis();
       float total_time = (end_time - start_time) / 1000.0; 
       Serial.printf(">>> THOI GIAN NAP HOAN TAT: %.2f giay <<<\n", total_time);

       return true;
    }
    return false;
}

void setup()
{
    Serial.begin(115200);
    Serial2.begin(115200);

    pinMode(STM32_RESET_PIN, OUTPUT);
    digitalWrite(STM32_RESET_PIN, HIGH); 

    if(!SPIFFS.begin(true)){
        Serial.println("An Error has occurred while mounting SPIFFS");
        return;
    }

    WiFi.begin(ssid, password);
    Serial.print("WiFi connecting...");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected. IP:");
    Serial.println(WiFi.localIP());

    // 1. GIAO DIỆN TRANG CHỦ (CHỌN FILE VÀ UPLOAD LÊN ESP32)
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", R"rawliteral(
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>OTA Update | ESP32 to STM32</title>
      <style>
        :root { --primary: #007bff; --bg: #f4f7f6; --text: #333; }
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: var(--bg); color: var(--text); display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
        .card { background: #fff; padding: 40px 30px; border-radius: 16px; box-shadow: 0 10px 30px rgba(0,0,0,0.08); text-align: center; width: 100%; max-width: 400px; }
        h2 { margin-top: 0; color: #2c3e50; font-weight: 600; margin-bottom: 5px; }
        p.subtitle { color: #7f8c8d; font-size: 14px; margin-bottom: 25px; }
        .file-upload-wrapper { position: relative; border: 2px dashed #cbd5e1; border-radius: 12px; padding: 40px 20px; cursor: pointer; transition: all 0.3s ease; background-color: #f8fafc; margin-bottom: 20px; }
        .file-upload-wrapper:hover { border-color: var(--primary); background-color: #f1f8ff; }
        .file-upload-input { position: absolute; left: 0; top: 0; height: 100%; width: 100%; opacity: 0; cursor: pointer; }
        .file-icon { font-size: 32px; margin-bottom: 10px; }
        .file-name { font-size: 14px; color: #64748b; font-weight: 500; }
        .btn { display: inline-block; width: 100%; padding: 14px; border-radius: 8px; border: none; font-size: 16px; font-weight: 600; cursor: pointer; transition: all 0.3s ease; color: #fff; text-decoration: none; box-sizing: border-box; }
        .btn-primary { background-color: var(--primary); box-shadow: 0 4px 12px rgba(0,123,255,0.2); }
        .btn-primary:hover { background-color: #0069d9; transform: translateY(-2px); box-shadow: 0 6px 15px rgba(0,123,255,0.3); }
        .btn:disabled { background-color: #94a3b8; cursor: not-allowed; transform: none; box-shadow: none; }
      </style>
    </head>
    <body>
      <div class="card">
        <h2>OTA Update System</h2>
        <p class="subtitle">Step 1: Upload firmware to ESP32</p>
        <form id="uploadForm" method="POST" action="/upload" enctype="multipart/form-data">
          <div class="file-upload-wrapper">
            <input type="file" name="file" accept=".bin" class="file-upload-input" id="fileInput" required>
            <div class="file-icon">📁</div>
            <div class="file-name" id="fileName">Click to choose .BIN file</div>
          </div>
          <button type="submit" class="btn btn-primary" id="uploadBtn">Upload to ESP32</button>
        </form>
      </div>
      <script>
        const fileInput = document.getElementById('fileInput');
        const fileName = document.getElementById('fileName');
        const uploadBtn = document.getElementById('uploadBtn');
        const form = document.getElementById('uploadForm');

        fileInput.addEventListener('change', function(e) {
          if (e.target.files.length > 0) {
            fileName.textContent = e.target.files[0].name;
            fileName.style.color = '#007bff';
          } else {
            fileName.textContent = 'Click to choose .BIN file';
            fileName.style.color = '#64748b';
          }
        });

        form.addEventListener('submit', function() {
          uploadBtn.textContent = 'Uploading... Please wait';
          uploadBtn.disabled = true;
        });
      </script>
    </body>
    </html>
  )rawliteral");
    });

    // 2. GIAO DIỆN SAU KHI UPLOAD LÊN ESP32 THÀNH CÔNG (CHUẨN BỊ FLASH XUỐNG STM32)
    server.on("/upload", HTTP_POST, []() {
        server.send(200, "text/html", R"rawliteral(
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>Firmware Uploaded</title>
      <style>
        :root { --success: #10b981; --bg: #f4f7f6; --text: #333; }
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: var(--bg); color: var(--text); display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
        .card { background: #fff; padding: 40px 30px; border-radius: 16px; box-shadow: 0 10px 30px rgba(0,0,0,0.08); text-align: center; width: 100%; max-width: 400px; }
        .icon-circle { width: 70px; height: 70px; background-color: #d1fae5; border-radius: 50%; display: flex; align-items: center; justify-content: center; font-size: 32px; color: var(--success); margin: 0 auto 20px auto; }
        h2 { margin-top: 0; color: #2c3e50; font-weight: 600; margin-bottom: 5px; }
        p.subtitle { color: #64748b; font-size: 15px; margin-bottom: 30px; line-height: 1.5; }
        .btn { display: inline-block; width: 100%; padding: 14px; border-radius: 8px; border: none; font-size: 16px; font-weight: 600; cursor: pointer; transition: all 0.3s ease; color: #fff; text-decoration: none; box-sizing: border-box; }
        .btn-success { background-color: var(--success); box-shadow: 0 4px 12px rgba(16, 185, 129, 0.2); }
        .btn-success:hover { background-color: #059669; transform: translateY(-2px); box-shadow: 0 6px 15px rgba(16, 185, 129, 0.3); }
        .btn.disabled { background-color: #94a3b8; cursor: not-allowed; transform: none; box-shadow: none; pointer-events: none; }
      </style>
    </head>
    <body>
      <div class="card">
        <div class="icon-circle">✓</div>
        <h2>ESP32 Ready</h2>
        <p class="subtitle">Firmware has been successfully uploaded to ESP32's SPIFFS memory.<br>Click below to start STM32 flashing.</p>
        <a href="/send" class="btn btn-success" id="flashBtn" onclick="startFlashing()">Flash to STM32</a>
      </div>
      <script>
        function startFlashing() {
          const btn = document.getElementById('flashBtn');
          btn.innerHTML = 'Flashing STM32... DO NOT CLOSE';
          btn.classList.add('disabled');
        }
      </script>
    </body>
    </html>
  )rawliteral");
    }, handleUpload);

    // 3. GIAO DIỆN KẾT QUẢ CUỐI CÙNG SAU KHI FLASH STM32
    server.on("/send", HTTP_GET, []() {
        bool success = sendFirmwareToSTM32();
        
        // Render màu sắc và icon dựa vào kết quả
        String icon = success ? "✓" : "✗";
        String colorTheme = success ? "#10b981" : "#ef4444"; 
        String bgTheme = success ? "#d1fae5" : "#fee2e2";
        String title = success ? "Success!" : "Flash Failed";
        String message = success ? "Firmware sent and verified by STM32 successfully." : "An error occurred during UART transmission or Checksum failed.";

        server.send(200, "text/html", R"rawliteral(
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>Flash Result</title>
      <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f4f7f6; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
        .card { background: #fff; padding: 40px 30px; border-radius: 16px; box-shadow: 0 10px 30px rgba(0,0,0,0.08); text-align: center; width: 100%; max-width: 400px; }
        .icon-circle { width: 70px; height: 70px; background-color: )rawliteral" + bgTheme + R"rawliteral(; border-radius: 50%; display: flex; align-items: center; justify-content: center; font-size: 36px; color: )rawliteral" + colorTheme + R"rawliteral(; margin: 0 auto 20px auto; font-weight: bold; }
        h2 { margin-top: 0; color: #2c3e50; font-weight: 600; margin-bottom: 10px; }
        p.subtitle { color: #64748b; font-size: 15px; margin-bottom: 30px; line-height: 1.5; }
        .btn { display: inline-block; width: 100%; padding: 14px; border-radius: 8px; border: none; font-size: 16px; font-weight: 600; cursor: pointer; transition: all 0.3s ease; color: #fff; text-decoration: none; box-sizing: border-box; background-color: #64748b; }
        .btn:hover { background-color: #475569; transform: translateY(-2px); box-shadow: 0 6px 15px rgba(100, 116, 139, 0.3); }
      </style>
    </head>
    <body>
      <div class="card">
        <div class="icon-circle">)rawliteral" + icon + R"rawliteral(</div>
        <h2>)rawliteral" + title + R"rawliteral(</h2>
        <p class="subtitle">)rawliteral" + message + R"rawliteral(</p>
        <a href="/" class="btn">Upload Another Firmware</a>
      </div>
    </body>
    </html>
  )rawliteral");
    });

    server.begin();
}

void loop()
{
    server.handleClient();
}