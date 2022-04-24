#include "main.h"
#include "version.h"

#include "html/h/style_css.h"
#include "html/h/setup_html.h"


//-----------------------------------------------------------------------------
Main::Main(void) {
    mDns     = new DNSServer();
    mWeb     = new ESP8266WebServer(80);
    mUpdater = new ESP8266HTTPUpdateServer();
    mUdp     = new WiFiUDP();

    mApActive          = true;
    mWifiSettingsValid = false;
    mSettingsValid     = false;

    snprintf(mVersion, 12, "%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);

    memset(&mDeviceName, 0, DEVNAME_LEN);

    mEep = new eep();
    Serial.begin(115200);

    mUptimeSecs = 0;
    mUptimeTicker = new Ticker();
    mUptimeTicker->attach(1, std::bind(&Main::uptimeTicker, this));

}


//-----------------------------------------------------------------------------
void Main::setup(const char *ssid, const char *pwd, uint32_t timeout) {
    bool startAp = mApActive;

    mWeb->on("/setup", std::bind(&Main::showSetup, this));
    mWeb->on("/save", std::bind(&Main::showSave, this));
    mWeb->on("/uptime", std::bind(&Main::showUptime, this));
    mWeb->on("/time", std::bind(&Main::showTime, this));
    mWeb->on("/style.css", std::bind(&Main::showCss, this));
    mWeb->on("/reboot", std::bind(&Main::showReboot,   this));
    mWeb->onNotFound (std::bind(&Main::showNotFound, this));

    startAp = getConfig();
    if(String(mDeviceName) != "")
        WiFi.hostname(mDeviceName);

    if(false == startAp)
        startAp = setupStation(timeout);

    if(true == startAp) {
        if(strlen(pwd) < 8)
            Serial.println("password must be at least 8 characters long");
        setupAp(ssid, pwd);
    }

    mUpdater->setup(mWeb);

    mApActive = startAp;

    mTimestamp  = getNtpTime();
    //Serial.println("[NTP]: " + getDateTimeStr(getNtpTime()));
}


//-----------------------------------------------------------------------------
void Main::loop(void) {
    if(mApActive)
        mDns->processNextRequest();
    mWeb->handleClient();
}


//-----------------------------------------------------------------------------
bool Main::getConfig(void) {
    bool mApActive = false;

    mWifiSettingsValid = checkEEpCrc(ADDR_START, ADDR_WIFI_CRC, ADDR_WIFI_CRC);
    mSettingsValid = checkEEpCrc(ADDR_START_SETTINGS, (ADDR_NEXT-ADDR_START_SETTINGS), ADDR_SETTINGS_CRC);

    if(mWifiSettingsValid) {
        mEep->read(ADDR_SSID,    mStationSsid, SSID_LEN);
        mEep->read(ADDR_PWD,     mStationPwd, PWD_LEN);
        mEep->read(ADDR_DEVNAME, mDeviceName, DEVNAME_LEN);
    }
    else {
        mApActive = true;
        memset(mStationSsid, 0, SSID_LEN);
        memset(mStationPwd, 0, PWD_LEN);
        memset(mDeviceName, 0, DEVNAME_LEN);

        // erase eeprom
        uint8_t buf[ADDR_NEXT-ADDR_START_SETTINGS] = {0};
        mEep->write(ADDR_START_SETTINGS, buf, (ADDR_NEXT-ADDR_START_SETTINGS));
    }

    return mApActive;
}


//-----------------------------------------------------------------------------
void Main::setupAp(const char *ssid, const char *pwd) {
    IPAddress apIp(192, 168, 1, 1);

    Serial.println("\n---------\nAP MODE\nSSDI: "+ String(ssid) + "\nPWD: " + String(pwd) + "\n---------\n");

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid, pwd);

    mDns->start(mDnsPort, "*", apIp);

    mWeb->onNotFound([&]() {
        showSetup();
    });
    mWeb->on("/", std::bind(&Main::showSetup, this));

    mWeb->begin();
}


//-----------------------------------------------------------------------------
bool Main::setupStation(uint32_t timeout) {
    int32_t cnt = timeout * 10;
    bool startAp = false;

    WiFi.mode(WIFI_STA);
    WiFi.begin(mStationSsid, mStationPwd);

    delay(5000);
    Serial.println("wait for network");
    while (WiFi.status() != WL_CONNECTED) {
        delay(100);
        if(cnt % 100 == 0)
            Serial.println(".");
        else
            Serial.print(".");

        if(timeout > 0) { // limit == 0 -> no limit
            if(--cnt <= 0) {
                startAp = true;
                WiFi.disconnect();
                delay(100);
                break;
            }
        }
    }
    Serial.println(".");

    if(false == startAp) {
        mWeb->begin();
    }

    delay(1000);

    return startAp;
}


//-----------------------------------------------------------------------------
void Main::showSetup(void) {
    String html = setup_html;
    html.replace("{SSID}", mStationSsid);
    // PWD will be left at the default value (for protection)
    // -> the PWD will only be changed if it does not match the default "{PWD}"
    html.replace("{DEVICE}", String(mDeviceName));
    html.replace("{VERSION}", String(mVersion));

    mWeb->send(200, "text/html", html);
}


//-----------------------------------------------------------------------------
void Main::showCss(void) {
    mWeb->send(200, "text/css", style_css);
}


//-----------------------------------------------------------------------------
void Main::showSave(void) {
    saveValues(true);
}


//-----------------------------------------------------------------------------
void Main::saveValues(bool webSend = true) {
    if(mWeb->args() > 0) {
        if(mWeb->arg("ssid") != "") {
            memset(mStationSsid, 0, SSID_LEN);
            mWeb->arg("ssid").toCharArray(mStationSsid, SSID_LEN);
            mEep->write(ADDR_SSID, mStationSsid, SSID_LEN);

            if(mWeb->arg("pwd") != "{PWD}") {
                memset(mStationPwd, 0, PWD_LEN);
                mWeb->arg("pwd").toCharArray(mStationPwd, PWD_LEN);
                mEep->write(ADDR_PWD, mStationPwd, PWD_LEN);
            }
        }

        memset(mDeviceName, 0, DEVNAME_LEN);
        mWeb->arg("device").toCharArray(mDeviceName, DEVNAME_LEN);
        mEep->write(ADDR_DEVNAME, mDeviceName, DEVNAME_LEN);


        updateCrc();
        if(webSend) {
            if(mWeb->arg("reboot") == "on")
                showReboot();
            else
                mWeb->send(200, "text/html", "<!doctype html><html><head><title>Setup saved</title><meta http-equiv=\"refresh\" content=\"0; URL=/setup\"></head><body>"
                             "<p>saved</p></body></html>");
        }
    }
}


//-----------------------------------------------------------------------------
void Main::updateCrc(void) {
    uint16_t crc;
    crc = buildEEpCrc(ADDR_START, ADDR_WIFI_CRC);
    //Serial.println("new CRC: " + String(crc, HEX));
    mEep->write(ADDR_WIFI_CRC, crc);
}


//-----------------------------------------------------------------------------
void Main::showUptime(void) {
    char time[20] = {0};

    int upTimeSc = uint32_t((mUptimeSecs) % 60);
    int upTimeMn = uint32_t((mUptimeSecs / (60)) % 60);
    int upTimeHr = uint32_t((mUptimeSecs / (60 * 60)) % 24);
    int upTimeDy = uint32_t((mUptimeSecs / (60 * 60 * 24)) % 365);

    snprintf(time, 20, "%d Tage, %02d:%02d:%02d", upTimeDy, upTimeHr, upTimeMn, upTimeSc);

    mWeb->send(200, "text/plain", String(time));
}


//-----------------------------------------------------------------------------
void Main::showTime(void) {
    mWeb->send(200, "text/plain", getDateTimeStr(mTimestamp));
}


//-----------------------------------------------------------------------------
void Main::showNotFound(void) {
    String msg = "File Not Found\n\n";
    msg += "URI: ";
    msg += mWeb->uri();
    msg += "\nMethod: ";
    msg += ( mWeb->method() == HTTP_GET ) ? "GET" : "POST";
    msg += "\nArguments: ";
    msg += mWeb->args();
    msg += "\n";

    for(uint8_t i = 0; i < mWeb->args(); i++ ) {
        msg += " " + mWeb->argName(i) + ": " + mWeb->arg(i) + "\n";
    }

    mWeb->send(404, "text/plain", msg);
}


//-----------------------------------------------------------------------------
void Main::showReboot(void) {
    mWeb->send(200, "text/html", "<!doctype html><html><head><title>Rebooting ...</title><meta http-equiv=\"refresh\" content=\"10; URL=/\"></head><body>rebooting ... auto reload after 10s</body></html>");
    delay(1000);
    ESP.restart();
}



//-----------------------------------------------------------------------------
void Main::uptimeTicker(void) {
    mUptimeSecs++;
    mTimestamp++;
}


//-----------------------------------------------------------------------------
time_t Main::getNtpTime(void) {
    time_t date = 0;
    IPAddress timeServer;
    uint8_t buf[NTP_PACKET_SIZE];
    uint8_t retry = 0;

    WiFi.hostByName (TIMESERVER_NAME, timeServer);
    mUdp->begin(TIME_LOCAL_PORT);


    sendNTPpacket(timeServer);

    while(retry++ < 5) {
        int wait = 150;
        while(--wait) {
            if(NTP_PACKET_SIZE <= mUdp->parsePacket()) {
                uint64_t secsSince1900;
                mUdp->read(buf, NTP_PACKET_SIZE);
                secsSince1900  = (buf[40] << 24);
                secsSince1900 |= (buf[41] << 16);
                secsSince1900 |= (buf[42] <<  8);
                secsSince1900 |= (buf[43]      );

                date = secsSince1900 - 2208988800UL; // UTC time
                date += (TIMEZONE + offsetDayLightSaving(date)) * 3600;
                break;
            }
            else
                delay(10);
        }
    }

    return date;
}


//-----------------------------------------------------------------------------
void Main::sendNTPpacket(IPAddress& address) {
    uint8_t buf[NTP_PACKET_SIZE] = {0};

    buf[0] = B11100011; // LI, Version, Mode
    buf[1] = 0;         // Stratum
    buf[2] = 6;         // Max Interval between messages in seconds
    buf[3] = 0xEC;      // Clock Precision
    // bytes 4 - 11 are for Root Delay and Dispersion and were set to 0 by memset
    buf[12] = 49;       // four-byte reference ID identifying
    buf[13] = 0x4E;
    buf[14] = 49;
    buf[15] = 52;

    mUdp->beginPacket(address, 123); // NTP request, port 123
    mUdp->write(buf, NTP_PACKET_SIZE);
    mUdp->endPacket();
}


//-----------------------------------------------------------------------------
String Main::getDateTimeStr(time_t t) {
    char str[20] = {0};
    sprintf(str, "%04d-%02d-%02d+%02d:%02d:%02d", year(t), month(t), day(t), hour(t), minute(t), second(t));
    return String(str);
}


//-----------------------------------------------------------------------------
// calculates the daylight saving time for middle Europe. Input: Unixtime in UTC
// from: https://forum.arduino.cc/index.php?topic=172044.msg1278536#msg1278536
time_t Main::offsetDayLightSaving (uint32_t local_t) {
    int m = month (local_t);
    if(m < 3 || m > 10) return 0; // no DSL in Jan, Feb, Nov, Dez
    if(m > 3 && m < 10) return 1; // DSL in Apr, May, Jun, Jul, Aug, Sep
    int y = year (local_t);
    int h = hour (local_t);
    int hToday = (h + 24 * day(local_t));
    if((m == 3  && hToday >= (1 + TIMEZONE + 24 * (31 - (5 * y /4 + 4) % 7)))
        || (m == 10 && hToday <  (1 + TIMEZONE + 24 * (31 - (5 * y /4 + 1) % 7))) )
        return 1;
    else
        return 0;
}
