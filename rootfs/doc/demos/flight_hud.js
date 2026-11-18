// Avionics Telemetry HUD demo for KestrelJS (/bin/kjs)
var win = Kestrel.openWindow("Avionics Telemetry HUD", 560, 380);
if (!win) {
    print("flight_hud: no window manager available");
} else {
    var hist = [], mem = { totalKb: 0, freeKb: 0 }, procs = [];
    var toast = "Click 'Pin Snapshot' to save telemetry to Talon";
    var toastUntil = 0, lastSample = -1000;

    function sample() {
        mem = Kestrel.memInfo() || { totalKb: 0, freeKb: 0 };
        procs = Kestrel.ps() || [];
        var used = mem.totalKb > 0 ? mem.totalKb - mem.freeKb : 0;
        var pct = mem.totalKb > 0 ? Math.round((used * 100) / mem.totalKb) : 0;
        hist.push(pct);
        if (hist.length > 36) hist.shift();
    }

    function draw() {
        var W = win.width, H = win.height, cardW = Math.floor((W - 36) / 2), cardH = H - 128;
        var usedMb = Math.round((mem.totalKb - mem.freeKb) / 1024), totalMb = Math.round(mem.totalKb / 1024);
        var pct = hist.length ? hist[hist.length - 1] : 0, running = 0, rx = 24 + cardW;
        for (var i = 0; i < procs.length; i++) if (procs[i].state === "running") running++;

        win.clear(0x0B111B);
        win.roundRect(12, 12, W - 24, 42, 8, 0x152031);
        win.text(24, 22, "KESTREL AVIONICS TELEMETRY HUD", 0x60A5FA, 16, true);
        win.text(W - 110, 25, "UP " + Math.floor(Kestrel.uptime() / 1000) + "s", 0x4ADE80, 16);

        win.roundRect(12, 66, cardW, cardH, 8, 0x111A28);
        win.text(24, 78, "MEMORY LOAD: " + pct + "%", 0xEDF3FA, 16);
        win.text(24, 100, usedMb + " / " + totalMb + " MiB", 0x8FA1BA, 12);
        win.sparkline(24, 124, cardW - 24, cardH - 72, hist, 0x60A5FA);

        win.roundRect(rx, 66, cardW, cardH, 8, 0x111A28);
        win.text(rx + 12, 78, "TASKS: " + procs.length + " (" + running + " active)", 0xEDF3FA, 16);
        for (var j = 0; j < procs.length && 108 + j * 18 < 66 + cardH - 12; j++) {
            var p = procs[j], col = p.state === "running" ? 0x4ADE80 : 0x8FA1BA;
            win.text(rx + 12, 108 + j * 18, p.pid + " " + p.name + " [" + p.state + "]", col, 12);
        }

        win.roundRect(12, H - 50, 156, 34, 6, 0x1E3A5F);
        win.text(28, H - 41, "Pin Snapshot", 0xF8FAFC, 16);
        win.roundRect(W - 104, H - 50, 92, 34, 6, 0x261B28);
        win.text(W - 86, H - 41, "Quit (Q)", 0xFB7185, 16);
        win.text(180, H - 39, (toastUntil && Kestrel.uptime() > toastUntil) ? "Esc/Q to exit" : toast, 0x8FA1BA, 12);
        win.flush();
    }

    while (!win.closed) {
        var now = Kestrel.uptime(), dirty = false;
        if (now - lastSample >= 250) { sample(); lastSample = now; dirty = true; }
        var ev = win.pollEvent(30);
        if (ev) {
            if (ev.type === "close") break;
            if (ev.type === "resize") dirty = true;
            if (ev.type === "key" && (ev.key === 27 || ev.key === 113 || ev.key === 81)) { win.close(); break; }
            if (ev.type === "down" && ev.y >= win.height - 50 && ev.y <= win.height - 16) {
                if (ev.x >= 12 && ev.x <= 168) {
                    var snap = "HUD up=" + Math.floor(now / 1000) + "s mem=" + (hist[hist.length - 1] || 0) + "% procs=" + procs.length;
                    Kestrel.parcelPut("text", "HUD Snapshot", snap);
                    toast = "Pinned: " + snap; toastUntil = now + 3500; dirty = true;
                } else if (ev.x >= win.width - 104 && ev.x <= win.width - 12) { win.close(); break; }
            }
        }
        if (dirty) draw();
    }
}
