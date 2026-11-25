// PhoneVR.h - turns an ordinary phone into a VR headset, with no app to install.
//
//   PC  ----(MJPEG video: the Cardboard image)---->  phone browser, in a Cardboard viewer
//   PC  <---(head orientation, 30x a second)--------  phone motion sensors (DeviceOrientation)
//
// A tiny HTTP server (one thread per connection, plain sockets):
//   GET /            the phone page (full-screen image + sensor code)
//   GET /stream      multipart/x-mixed-replace JPEG stream ("motion JPEG")
//   GET /o?f=..&u=..&w=..   the phone's forward and up vectors in world space, w = screen held (walk)
//
// Phone and PC must be on the same Wi-Fi. Chrome gives motion sensors only to secure pages, so on
// plain http the page explains the one-time chrome://flags setting (or: adb reverse + localhost);
// without it the phone can still look around by dragging on the screen.
#pragma once
#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX                 // keep std::min / std::max usable
  #endif
  #if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
  #undef _WIN32_WINNT
  #define _WIN32_WINNT 0x0600      // inet_pton / inet_ntop
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define VC_BADSOCK INVALID_SOCKET
  #define VC_CLOSE closesocket
  #define VC_SENDFLAGS 0
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int sock_t;
  #define VC_BADSOCK (-1)
  #define VC_CLOSE ::close
  #define VC_SENDFLAGS MSG_NOSIGNAL
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <glm/glm.hpp>
#include "stb_image_write.h"   // implementation compiled in main.cpp

class PhoneVR {
public:
    int port = 8080;
    int quality = 70;          // JPEG quality
    float maxFps = 24.0f;      // frames sent per second at most

    ~PhoneVR() { stop(); }

    bool start(int wantPort = 8080) {
        if (running) return true;
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error = "Winsock failed to start"; return false; }
#endif
        for (int p = wantPort; p < wantPort + 10; ++p) {       // first free port
            listener = socket(AF_INET, SOCK_STREAM, 0);
            if (listener == VC_BADSOCK) { error = "socket() failed"; return false; }
            int yes = 1;
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
            sockaddr_in a{};
            a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((unsigned short)p);
            if (bind(listener, (sockaddr*)&a, sizeof a) == 0 && listen(listener, 8) == 0) { port = p; break; }
            VC_CLOSE(listener); listener = VC_BADSOCK;
        }
        if (listener == VC_BADSOCK) { error = "no free port"; return false; }
        running = true;
        ip = localIP();
        acceptThread = std::thread([this] { acceptLoop(); });
        error.clear();
        return true;
    }

    void stop() {
        if (!running) return;
        running = false;
        frameCv.notify_all();
        if (acceptThread.joinable()) acceptThread.join();
        VC_CLOSE(listener); listener = VC_BADSOCK;
        // connection threads are detached; they end on their own once `running` is false
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    bool isRunning() const { return running; }
    std::string url() const { return "http://" + ip + ":" + std::to_string(port); }

    // ---- head tracking, read by the main thread
    bool tracking() const { return running && secondsSince(lastPose) < 1.0; }
    bool streaming() const { return running && viewers > 0; }
    glm::vec3 forward() const { std::lock_guard<std::mutex> l(poseMx); return f; }
    glm::vec3 up() const { std::lock_guard<std::mutex> l(poseMx); return u; }
    bool walkHeld() const { return tracking() && walk; }
    float streamFps() const { return fps; }
    bool takeRecenter() { return recenterReq.exchange(false); }

    // ---- video, from the main thread: is a new frame wanted? then hand over the RGB pixels (bottom-up)
    bool wantsFrame() const { return streaming() && secondsSince(lastFrame) >= 1.0 / maxFps; }
    void submit(const unsigned char* rgbBottomUp, int w, int h) {
        {
            std::lock_guard<std::mutex> l(frameMx);
            frame.resize(size_t(w) * h * 3);
            for (int y = 0; y < h; ++y)                   // OpenGL rows run bottom-up, JPEG top-down
                std::memcpy(&frame[size_t(y) * w * 3], rgbBottomUp + size_t(h - 1 - y) * w * 3, size_t(w) * 3);
            fw = w; fh = h; ++frameSeq;
        }
        double dt = secondsSince(lastFrame);
        if (dt < 1.0) fps = fps * 0.9f + 0.1f * float(1.0 / std::max(dt, 1e-3));
        lastFrame = now();
        frameCv.notify_all();
    }

    std::string error;

private:
    sock_t listener = VC_BADSOCK;
    std::atomic<bool> running{false};
    std::thread acceptThread;
    std::string ip = "127.0.0.1";

    mutable std::mutex poseMx;
    glm::vec3 f{0, 0, -1}, u{0, 1, 0};
    std::atomic<bool> walk{false};
    std::atomic<bool> recenterReq{false};
    std::atomic<double> lastPose{-100.0};

    std::mutex frameMx;
    std::condition_variable frameCv;
    std::vector<unsigned char> frame;
    int fw = 0, fh = 0;
    unsigned long frameSeq = 0;
    std::atomic<double> lastFrame{-100.0};
    std::atomic<int> viewers{0};
    std::atomic<float> fps{0};

    static double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    static double secondsSince(double t) { return now() - t; }

    // The address other devices on the Wi-Fi use to reach this PC: "connect" a UDP socket
    // to a public address (no packet is sent) and ask which local address the OS chose.
    static std::string localIP() {
        sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == VC_BADSOCK) return "127.0.0.1";
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons(80);
        inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
        std::string r = "127.0.0.1";
        if (connect(s, (sockaddr*)&a, sizeof a) == 0) {
            sockaddr_in me{}; socklen_t len = sizeof me;
            if (getsockname(s, (sockaddr*)&me, &len) == 0) {
                char buf[64];
                if (inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf)) r = buf;
            }
        }
        VC_CLOSE(s);
        return r;
    }

    static bool sendAll(sock_t s, const void* data, size_t n) {
        const char* p = (const char*)data;
        while (n > 0) {
            int k = send(s, p, (int)std::min<size_t>(n, 1 << 20), VC_SENDFLAGS);
            if (k <= 0) return false;
            p += k; n -= size_t(k);
        }
        return true;
    }
    static bool sendStr(sock_t s, const std::string& t) { return sendAll(s, t.data(), t.size()); }

    void acceptLoop() {
        while (running) {
            // wait at most 200 ms, so stop() never hangs (closing a socket does not wake accept() on Linux)
            fd_set set; FD_ZERO(&set); FD_SET(listener, &set);
            timeval tv{0, 200000};
            if (select(int(listener) + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
            sockaddr_in a{}; socklen_t len = sizeof a;
            sock_t c = accept(listener, (sockaddr*)&a, &len);
            if (c == VC_BADSOCK) { if (!running) break; continue; }
            int one = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);   // low latency for small replies
#ifdef _WIN32
            DWORD to = 5000;
#else
            timeval to{5, 0};
#endif
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);   // idle connections end
            std::thread([this, c] { handle(c); }).detach();
        }
    }

    // value of key in "a=1&b=2"
    static std::string param(const std::string& q, const std::string& key) {
        size_t p = 0;
        while (p < q.size()) {
            size_t e = q.find('&', p); if (e == std::string::npos) e = q.size();
            size_t eq = q.find('=', p);
            if (eq != std::string::npos && eq < e && q.compare(p, eq - p, key) == 0) return q.substr(eq + 1, e - eq - 1);
            p = e + 1;
        }
        return "";
    }

    void handle(sock_t c) {
        // keep-alive: the phone sends many small /o requests on one connection
        std::string buf;
        char tmp[2048];
        while (running) {
            size_t end;
            while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
                int k = recv(c, tmp, sizeof tmp, 0);
                if (k <= 0 || buf.size() > 16384) { VC_CLOSE(c); return; }
                buf.append(tmp, size_t(k));
            }
            std::string head = buf.substr(0, end);
            buf.erase(0, end + 4);
            size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) break;
            std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
            std::string path = target.substr(0, target.find('?'));
            std::string query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);

            if (path == "/o") {
                if (param(query, "recenter") == "1") recenterReq = true;
                glm::vec3 nf, nu;
                if (std::sscanf(param(query, "f").c_str(), "%f,%f,%f", &nf.x, &nf.y, &nf.z) == 3 &&
                    std::sscanf(param(query, "u").c_str(), "%f,%f,%f", &nu.x, &nu.y, &nu.z) == 3 &&
                    glm::length(nf) > 0.5f && glm::length(nu) > 0.5f) {
                    std::lock_guard<std::mutex> l(poseMx);
                    f = glm::normalize(nf); u = glm::normalize(nu);
                    walk = param(query, "w") == "1";
                    lastPose = now();
                }
                if (!sendStr(c, "HTTP/1.1 204 No Content\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n\r\n")) break;
            } else if (path == "/stream") {
                stream(c);
                return;                                   // stream() closes the socket
            } else if (path == "/" || path == "/index.html") {
                std::string body = page();
                if (!sendStr(c, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
                                "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body)) break;
            } else {
                if (!sendStr(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n")) break;
            }
        }
        VC_CLOSE(c);
    }

    static void appendJpeg(void* ctx, void* data, int size) {
        auto* v = (std::vector<unsigned char>*)ctx;
        v->insert(v->end(), (unsigned char*)data, (unsigned char*)data + size);
    }

    void stream(sock_t c) {
        ++viewers;
        bool ok = sendStr(c, "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nConnection: close\r\n"
                             "Content-Type: multipart/x-mixed-replace; boundary=vcframe\r\n\r\n");
        unsigned long seen = 0;
        std::vector<unsigned char> rgb, jpg;
        int w = 0, h = 0;
        while (ok && running) {
            {
                std::unique_lock<std::mutex> l(frameMx);
                frameCv.wait_for(l, std::chrono::milliseconds(500), [&] { return !running || frameSeq != seen; });
                if (!running) break;
                if (frameSeq == seen) continue;
                seen = frameSeq; rgb = frame; w = fw; h = fh;
            }
            jpg.clear();
            stbi_write_jpg_to_func(appendJpeg, &jpg, w, h, 3, rgb.data(), quality);   // encode outside the lock
            ok = sendStr(c, "--vcframe\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpg.size()) + "\r\n\r\n") &&
                 sendAll(c, jpg.data(), jpg.size()) && sendStr(c, "\r\n");
        }
        --viewers;
        VC_CLOSE(c);
    }

    // The phone page. Orientation maths follows three.js DeviceOrientationControls: the device angles
    // become a quaternion, then the phone's forward (-z) and up (+y) are sent in world space
    // (x east, y up, z south - the same axes as the campus).
    static std::string page() {
        return R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="mobile-web-app-capable" content="yes">
<title>Virtual Campus VR</title>
<style>
html,body{margin:0;height:100%;background:#000;overflow:hidden;touch-action:none;font-family:sans-serif}
#v{position:fixed;inset:0;width:100%;height:100%;object-fit:contain;display:none}
#start{position:fixed;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;color:#eee;text-align:center;padding:12px;box-sizing:border-box;overflow:auto;font-size:14px}
h2{margin:4px 0;font-size:20px}
button{font-size:20px;padding:12px 26px;border-radius:12px;border:0;background:#3b7be0;color:#fff}
#hud{position:fixed;left:8px;bottom:6px;color:#8f8;font-size:12px;opacity:.7}
p{max-width:640px;line-height:1.35;margin:6px 0}
</style></head><body>
<img id="v" alt="">
<div id="start">
 <h2>Virtual Campus - phone VR</h2>
 <p>Turn the phone sideways and put it in a Cardboard viewer (or just hold it).<br>
 Look around by moving your head. <b>Touch and hold</b> the screen to walk where you look.<br>
 Double-tap to re-centre.</p>
 <button id="go">Start VR</button>
 <p id="msg" style="color:#fc8"></p>
</div>
<div id="hud"></div>
<script>
const img=document.getElementById('v'), hud=document.getElementById('hud'), msg=document.getElementById('msg');
let ori=null, walk=false, sensor=false, dragYaw=0, dragPitch=0, lastSend=0, busy=false, lastTap=0;
if(!window.isSecureContext){
 msg.innerHTML='Chrome blocks motion sensors on plain http pages, so head tracking needs one setting (once):<br>'+
 '1. In Chrome open <b>chrome://flags/#unsafely-treat-insecure-origin-as-secure</b><br>'+
 '2. Type <b>'+location.origin+'</b> in the box, set it to <b>Enabled</b>, tap <b>Relaunch</b>.<br>'+
 'Without it you can still drag on the screen to look around.';
}
function screenAngle(){ return (screen.orientation&&screen.orientation.angle)||window.orientation||0; }
// quaternion helpers [x,y,z,w]
function qmul(a,b){return[a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
 a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]];}
function qrot(q,v){const[x,y,z,w]=q,ix=w*v[0]+y*v[2]-z*v[1],iy=w*v[1]+z*v[0]-x*v[2],iz=w*v[2]+x*v[1]-y*v[0],iw=-x*v[0]-y*v[1]-z*v[2];
 return[ix*w+iw*-x+iy*-z-iz*-y,iy*w+iw*-y+iz*-x-ix*-z,iz*w+iw*-z+ix*-y-iy*-x];}
function deviceQuat(a,b,g,orient){ // radians; euler order YXZ (x=beta, y=alpha, z=-gamma)
 const x=b/2,y=a/2,z=-g/2,c1=Math.cos(x),c2=Math.cos(y),c3=Math.cos(z),s1=Math.sin(x),s2=Math.sin(y),s3=Math.sin(z);
 let q=[s1*c2*c3+c1*s2*s3,c1*s2*c3-s1*c2*s3,c1*c2*s3-s1*s2*c3,c1*c2*c3+s1*s2*s3];
 q=qmul(q,[-Math.SQRT1_2,0,0,Math.SQRT1_2]);            // camera looks out of the back of the phone
 q=qmul(q,[0,0,Math.sin(-orient/2),Math.cos(-orient/2)]); // landscape / portrait
 return q;}
function pose(){
 if(sensor&&ori){const d=Math.PI/180;const q=deviceQuat(ori.alpha*d,ori.beta*d,ori.gamma*d,screenAngle()*d);
  return[qrot(q,[0,0,-1]),qrot(q,[0,1,0])];}
 const cy=Math.cos(dragYaw),sy=Math.sin(dragYaw),cp=Math.cos(dragPitch),sp=Math.sin(dragPitch); // touch fallback
 return[[sy*cp,sp,-cy*cp],[-sy*sp,cp,cy*sp]];}
function send(){
 requestAnimationFrame(send);
 const t=performance.now(); if(busy||t-lastSend<33) return;
 const [f,u]=pose(); busy=true; lastSend=t;
 const r=v=>v.map(n=>n.toFixed(4)).join(',');
 fetch('/o?f='+r(f)+'&u='+r(u)+'&w='+(walk?1:0),{cache:'no-store'}).catch(()=>{}).finally(()=>busy=false);
 hud.textContent=(sensor?'head tracking':'drag to look (no motion sensor)')+(walk?' - walking':'');}
window.addEventListener('deviceorientation',e=>{if(e.alpha!==null){ori=e;sensor=true;}});
let lx=0,ly=0;
document.addEventListener('touchstart',e=>{const t=performance.now();
 if(t-lastTap<300) fetch('/o?recenter=1').catch(()=>{}); lastTap=t;
 walk=true; lx=e.touches[0].clientX; ly=e.touches[0].clientY;},{passive:true});
document.addEventListener('touchmove',e=>{ if(sensor) return; const x=e.touches[0].clientX,y=e.touches[0].clientY;
 dragYaw+=(x-lx)*0.006; dragPitch=Math.max(-1.4,Math.min(1.4,dragPitch-(y-ly)*0.006)); lx=x; ly=y; walk=false;},{passive:true});
document.addEventListener('touchend',()=>walk=false);
document.getElementById('go').onclick=async()=>{
 try{ if(typeof DeviceOrientationEvent!=='undefined'&&DeviceOrientationEvent.requestPermission)
   await DeviceOrientationEvent.requestPermission(); }catch(e){}
 try{ await document.documentElement.requestFullscreen(); }catch(e){}
 try{ await screen.orientation.lock('landscape'); }catch(e){}
 try{ if('wakeLock' in navigator) await navigator.wakeLock.request('screen'); }catch(e){}
 document.getElementById('start').style.display='none'; img.style.display='block';
 img.src='/stream?'+Date.now();
 img.onerror=()=>{setTimeout(()=>img.src='/stream?'+Date.now(),1000);};
 setTimeout(()=>{ if(!sensor) hud.textContent='No motion sensor data: drag to look. See README for the Chrome flag.'; },2500);
 send();};
</script></body></html>)HTML";
    }
};
