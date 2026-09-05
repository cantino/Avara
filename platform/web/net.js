// Datagram transport for the web build, loaded ahead of the module as --pre-js.
//
// The engine's connect, level-gather and resume paths all spin in place waiting
// for the other players to answer. On a browser's main thread nothing can
// arrive while such a loop runs -- the WebSocket's message events are queued
// behind the loop that is waiting for them -- so those loops always time out.
//
// So the socket lives on a worker instead, and datagrams cross into the main
// thread through a SharedArrayBuffer ring that can be read synchronously. That
// makes receiving work exactly like a real UDP socket from the engine's point
// of view, and every one of those loops works unchanged.
//
// SharedArrayBuffer needs the page to be cross-origin isolated (COOP + COEP).
// When it isn't, this falls back to a WebSocket on the main thread: fine for
// single player and for sitting in a lobby, but a match will not start.
(function () {
    var SLOTS = 512;      // ring capacity in datagrams
    var SLOT = 2048;      // bytes per slot: 4 length + 6 source address + payload
    var HEAD = 10;        // length + source address
    var DATA_OFF = 64;    // control block occupies the first 64 bytes
    var SAB_SIZE = DATA_OFF + SLOTS * SLOT;

    var C_WRITE = 0, C_READ = 1, C_HOST = 2, C_PORT = 3, C_STATE = 4;
    var MASK = 0x3fffffff;  // keep the indices well clear of int32 overflow

    // Runs on the worker. Owns the WebSocket; the main thread never touches it.
    var WORKER_SRC = [
        'var SLOTS=' + SLOTS + ',SLOT=' + SLOT + ',HEAD=' + HEAD + ',DATA_OFF=' + DATA_OFF + ';',
        'var C_WRITE=' + C_WRITE + ',C_READ=' + C_READ + ',C_HOST=' + C_HOST +
            ',C_PORT=' + C_PORT + ',C_STATE=' + C_STATE + ',MASK=' + MASK + ';',
        'var ctrl,i32,u8,ws,port=0,open=false,outQ=[];',
        'function hello(){var h=new Uint8Array(3);h[0]=1;h[1]=(port>>8)&255;h[2]=port&255;ws.send(h);}',
        'function push(b){',
        '  var len=b.length-7;',
        '  if(len<0||len>SLOT-HEAD){return;}',
        '  var w=Atomics.load(ctrl,C_WRITE),r=Atomics.load(ctrl,C_READ);',
        '  if(((w-r)&MASK)>=SLOTS){return;}',            // full: drop, like a real socket
        '  var off=DATA_OFF+(w%SLOTS)*SLOT;',
        '  i32[off>>2]=len;',
        '  u8.set(b.subarray(1,7),off+4);',
        '  u8.set(b.subarray(7,7+len),off+HEAD);',
        '  Atomics.store(ctrl,C_WRITE,(w+1)&MASK);',
        '}',
        'onmessage=function(ev){',
        '  var m=ev.data;',
        '  if(m.cmd==="init"){',
        '    ctrl=new Int32Array(m.sab,0,16);',
        '    i32=new Int32Array(m.sab);',
        '    u8=new Uint8Array(m.sab);',
        '    port=m.port;',
        '    ws=new WebSocket(m.url);',
        '    ws.binaryType="arraybuffer";',
        '    ws.onopen=function(){',
        '      open=true;hello();',
        '      for(var i=0;i<outQ.length;i++){ws.send(outQ[i]);}',
        '      outQ.length=0;',
        '      Atomics.store(ctrl,C_STATE,1);',
        '    };',
        '    ws.onmessage=function(e){',
        '      var b=new Uint8Array(e.data);',
        '      if(b.length<7){return;}',
        '      if(b[0]===0x81){',
        '        var h=((b[1]<<24)|(b[2]<<16)|(b[3]<<8)|b[4])>>>0;',
        '        Atomics.store(ctrl,C_HOST,h|0);',
        '        Atomics.store(ctrl,C_PORT,(b[5]<<8)|b[6]);',
        '        postMessage({welcome:h});',
        '      } else if(b[0]===0x82){push(b);}',
        '    };',
        '    ws.onclose=function(){open=false;Atomics.store(ctrl,C_STATE,2);};',
        '    ws.onerror=function(){open=false;Atomics.store(ctrl,C_STATE,2);};',
        '  } else if(m.cmd==="bind"){',
        '    port=m.port; if(open){hello();}',
        '  } else if(m.cmd==="send"){',
        '    var f=new Uint8Array(m.buf);',
        '    if(open){ws.send(f);} else if(outQ.length<256){outQ.push(f);}',
        '  }',
        '};'
    ].join('\n');

    function gatewayURL() {
        if (typeof AVARA_GATEWAY_URL !== 'undefined' && AVARA_GATEWAY_URL) {
            return AVARA_GATEWAY_URL;
        }
        if (typeof window !== 'undefined' && window.AVARA_GATEWAY_URL) {
            return window.AVARA_GATEWAY_URL;
        }
        return (location.protocol === 'https:' ? 'wss:' : 'ws:') + '//' + location.host + '/net';
    }

    // The room code is the low 24 bits of the assigned address in base32, short
    // enough to read out loud or paste into a link.
    function roomCode(host) {
        var alpha = '23456789abcdefghjkmnpqrstuvwxyz';
        var id = host & 0xffffff, code = '';
        for (var i = 0; i < 5; i++) {
            code = alpha[id % 31] + code;
            id = Math.floor(id / 31);
        }
        return code;
    }

    function announce(host) {
        var code = roomCode(host);
        if (typeof window !== 'undefined') {
            window.__avaraRoomCode = code;
            window.dispatchEvent(new CustomEvent('avara-room-code', {detail: code}));
        }
    }

    // A hidden tab gets no animation frames, so the game loop stops -- and in a
    // lockstep match that stalls every other player too. This drives the loop
    // from a worker while the page is hidden, without rendering: the simulation
    // and the network keep up, and there is nothing on screen to draw.
    //
    // The worker sleeps on Atomics.wait rather than a timer, because timers in
    // a background page are throttled to roughly one a second. Control is also
    // through that shared word, since a worker parked in Atomics.wait is not
    // running its event loop and cannot be told anything by postMessage.
    //   word 0: 1 while the page is hidden, 0 otherwise
    //   word 1: 1 while a tick is outstanding, so ticks cannot pile up faster
    //           than the main thread can run them
    var CLOCK_SRC = [
        'onmessage=function(e){',
        '  var c=new Int32Array(e.data.buf),period=e.data.period||16;',
        '  for(;;){',
        '    Atomics.wait(c,0,0);',                       // parked until hidden
        '    while(Atomics.load(c,0)===1){',
        '      if(Atomics.load(c,1)===0){',
        '        Atomics.store(c,1,1);',
        '        postMessage(1);',
        '      }',
        '      Atomics.wait(c,0,1,period);',              // wakes early if shown
        '    }',
        '  }',
        '};'
    ].join('\n');

    var clock = null;

    function startClock() {
        var buf = new SharedArrayBuffer(8);
        var c = new Int32Array(buf);
        var blob = new Blob([CLOCK_SRC], {type: 'text/javascript'});
        var w = new Worker(URL.createObjectURL(blob));
        w.onmessage = function () {
            try {
                if (typeof Module !== 'undefined' && Module._avara_web_tick) {
                    Module._avara_web_tick();
                }
            } catch (e) {
                Atomics.store(c, 0, 0);  // runtime is gone; stop asking
            }
            Atomics.store(c, 1, 0);
        };
        w.postMessage({buf: buf, period: 16});
        return {worker: w, c: c};
    }

    function watchVisibility() {
        if (typeof document === 'undefined' || !globalThis.AvaraNet.isolated()) { return; }
        document.addEventListener('visibilitychange', function () {
            if (!clock) { clock = startClock(); }
            Atomics.store(clock.c, 0, document.hidden ? 1 : 0);
            Atomics.notify(clock.c, 0);
        });
    }

    var A = null;

    function startWorker(port) {
        var sab = new SharedArrayBuffer(SAB_SIZE);
        var blob = new Blob([WORKER_SRC], {type: 'text/javascript'});
        var worker = new Worker(URL.createObjectURL(blob));
        var self = {
            mode: 'worker',
            worker: worker,
            ctrl: new Int32Array(sab, 0, 16),
            i32: new Int32Array(sab),
            u8: new Uint8Array(sab),
            port: port
        };
        worker.onmessage = function (ev) {
            if (ev.data && ev.data.welcome !== undefined) { announce(ev.data.welcome); }
        };
        worker.postMessage({cmd: 'init', sab: sab, port: port, url: gatewayURL()});
        return self;
    }

    // Used when the page is not cross-origin isolated. Receiving only happens
    // when the main thread yields, so blocking waits in the engine will fail.
    function startInline(port) {
        var self = {mode: 'inline', ws: null, ready: false, outQ: [], inQ: [],
                    host: 0, port: port};
        var ws = new WebSocket(gatewayURL());
        ws.binaryType = 'arraybuffer';
        self.ws = ws;
        ws.onopen = function () {
            self.ready = true;
            var h = new Uint8Array(3);
            h[0] = 1; h[1] = (self.port >> 8) & 255; h[2] = self.port & 255;
            ws.send(h);
            for (var i = 0; i < self.outQ.length; i++) { ws.send(self.outQ[i]); }
            self.outQ.length = 0;
        };
        ws.onmessage = function (e) {
            var b = new Uint8Array(e.data);
            if (b.length < 7) { return; }
            if (b[0] === 0x81) {
                self.host = ((b[1] << 24) | (b[2] << 16) | (b[3] << 8) | b[4]) >>> 0;
                self.port = (b[5] << 8) | b[6];
                announce(self.host);
            } else if (b[0] === 0x82) {
                self.inQ.push(b);
            }
        };
        ws.onclose = ws.onerror = function () { self.ready = false; };
        return self;
    }

    globalThis.AvaraNet = {
        isolated: function () {
            return typeof SharedArrayBuffer !== 'undefined' &&
                   typeof Worker !== 'undefined' &&
                   (typeof crossOriginIsolated === 'undefined' || crossOriginIsolated);
        },

        // The gateway session outlives any one CUDPComm: Avara tears its socket
        // down and builds a new one on every net mode change, and reconnecting
        // would churn the session, and with it the room code, for no reason.
        bind: function (port) {
            if (A) {
                A.port = port;
                if (A.mode === 'worker') {
                    A.worker.postMessage({cmd: 'bind', port: port});
                } else if (A.ready) {
                    var h = new Uint8Array(3);
                    h[0] = 1; h[1] = (port >> 8) & 255; h[2] = port & 255;
                    A.ws.send(h);
                }
                return;
            }
            if (this.isolated()) {
                A = startWorker(port);
                watchVisibility();
            } else {
                console.warn('avara: page is not cross-origin isolated, so the ' +
                             'network transport cannot be read synchronously; ' +
                             'matches will not start. Serve with COOP/COEP headers.');
                A = startInline(port);
            }
        },

        // Drop anything still queued for the socket being torn down so it cannot
        // reach whatever CUDPComm is built next. The session itself stays up.
        unbind: function () {
            if (!A) { return; }
            if (A.mode === 'worker') {
                Atomics.store(A.ctrl, C_READ, Atomics.load(A.ctrl, C_WRITE));
            } else {
                A.inQ.length = 0;
                A.outQ.length = 0;
            }
        },

        status: function () {
            if (!A) { return -1; }
            if (A.mode === 'worker') { return 100 + Atomics.load(A.ctrl, C_STATE); }
            return (A.ready ? 100 : 0) + (A.ws ? A.ws.readyState : 9);
        },

        localHost: function () {
            if (!A) { return 0; }
            if (A.mode === 'worker') { return Atomics.load(A.ctrl, C_HOST) >>> 0; }
            return A.host >>> 0;
        },

        send: function (host, port, payload) {
            if (!A) { return; }
            var frame = new Uint8Array(7 + payload.length);
            frame[0] = 2;
            frame[1] = (host >>> 24) & 255; frame[2] = (host >>> 16) & 255;
            frame[3] = (host >>> 8) & 255;  frame[4] = host & 255;
            frame[5] = (port >> 8) & 255;   frame[6] = port & 255;
            frame.set(payload, 7);
            if (A.mode === 'worker') {
                A.worker.postMessage({cmd: 'send', buf: frame.buffer}, [frame.buffer]);
            } else if (A.ready) {
                A.ws.send(frame);
            } else if (A.outQ.length < 256) {
                A.outQ.push(frame);
            }
        },

        // Writes the payload at heap[bufPtr] and the source [ip:4][port:2] at
        // heap[srcPtr]; returns the payload length, or -1 if nothing is queued.
        recvInto: function (heap, bufPtr, maxLen, srcPtr) {
            if (!A) { return -1; }
            if (A.mode !== 'worker') {
                if (A.inQ.length === 0) { return -1; }
                var b = A.inQ.shift();
                var n = Math.min(b.length - 7, maxLen);
                heap.set(b.subarray(1, 7), srcPtr);
                heap.set(b.subarray(7, 7 + n), bufPtr);
                return n;
            }
            var w = Atomics.load(A.ctrl, C_WRITE), r = Atomics.load(A.ctrl, C_READ);
            if (w === r) { return -1; }
            var off = DATA_OFF + (r % SLOTS) * SLOT;
            var len = A.i32[off >> 2];
            if (len > maxLen) { len = maxLen; }
            heap.set(A.u8.subarray(off + 4, off + HEAD), srcPtr);
            heap.set(A.u8.subarray(off + HEAD, off + HEAD + len), bufPtr);
            Atomics.store(A.ctrl, C_READ, (r + 1) & MASK);
            return len;
        }
    };
})();
