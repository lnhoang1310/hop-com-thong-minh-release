/* Hộp cơm thông minh - trang cấu hình WiFi, máy chủ MQTT, cập nhật firmware
 * API: GET /api/status, GET /api/scan, POST /api/connect, POST /api/disconnect,
 *      GET|POST /api/mqtt, POST /api/ota (body = file .bin)
 */
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };
  var el = {
    card: $('statusCard'), label: $('stLabel'), ssid: $('stSsid'), signal: $('stSignal'),
    meta: $('stMeta'), ip: $('stIp'), rssi: $('stRssi'), actions: $('stActions'),
    btnDisc: $('btnDisconnect'), btnForget: $('btnForget'), btnReconnect: $('btnReconnect'),
    notice: $('notice'), nTitle: $('noticeTitle'), nBody: $('noticeBody'),
    list: $('netList'), btnScan: $('btnScan'),
    form: $('connectForm'), inSsid: $('ssid'), inPass: $('password'), btnShow: $('btnShowPass'),
    formErr: $('formError'), btnConnect: $('btnConnect'),
    ftAp: $('ftAp'), ftMac: $('ftMac'), ftFw: $('ftFw')
  };

  var STATUS_POLL_MS = 3000;
  var ATTEMPT_POLL_MS = 1000;
  var ATTEMPT_TIMEOUT_MS = 60000;

  var lastStatus = null;
  var pollTimer = null;
  var waitingAttempt = null;   /* {id, ssid, started} */
  var reconnecting = false;
  var scanning = false;
  var scanRetry = 0;
  var uploading = false;       /* đang tải firmware: tạm dừng hỏi trạng thái để không chiếm kết nối */

  /* ---------------- tiện ích ---------------- */
  function api(method, url, body, timeoutMs) {
    var ctrl = typeof AbortController !== 'undefined' ? new AbortController() : null;
    var t = ctrl ? setTimeout(function () { ctrl.abort(); }, timeoutMs || 8000) : null;
    var opt = { method: method, cache: 'no-store', headers: {} };
    if (ctrl) opt.signal = ctrl.signal;
    if (body !== undefined) {
      opt.headers['Content-Type'] = 'application/json';
      opt.body = JSON.stringify(body);
    }
    return fetch(url, opt).then(function (r) {
      if (t) clearTimeout(t);
      return r.json().catch(function () { return {}; }).then(function (data) {
        if (!r.ok) {
          var e = new Error(data.error || ('Lỗi ' + r.status));
          e.status = r.status;
          throw e;
        }
        return data;
      });
    }, function (err) {
      if (t) clearTimeout(t);
      var e = new Error('Mất kết nối với hộp cơm');
      e.network = true;
      e.cause = err;
      throw e;
    });
  }

  function rssiLevel(rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    return 1;
  }

  function rssiText(rssi) {
    return ['', 'Yếu', 'Trung bình', 'Tốt', 'Rất tốt'][rssiLevel(rssi)] + ' (' + rssi + ' dBm)';
  }

  function showNotice(kind, title, body) {
    el.notice.hidden = false;
    el.notice.dataset.kind = kind;
    el.nTitle.textContent = title;
    el.nBody.textContent = body || '';
  }

  function hideNotice() { el.notice.hidden = true; }

  function setBusy(btn, busy) {
    btn.disabled = busy;
    btn.classList.toggle('loading', busy);
  }

  function formError(msg) {
    el.formErr.hidden = !msg;
    el.formErr.textContent = msg || '';
  }

  /* ---------------- trạng thái ---------------- */
  var STATE_TEXT = {
    connected: 'Đã kết nối',
    connecting: 'Đang kết nối…',
    failed: 'Kết nối thất bại',
    disconnected: 'Đã ngắt kết nối',
    not_configured: 'Chưa kết nối WiFi'
  };

  function renderStatus(s) {
    lastStatus = s;
    el.card.dataset.state = s.state;
    el.label.textContent = STATE_TEXT[s.state] || s.state;

    var ssid = s.ssid || s.saved_ssid || '';
    if (s.state === 'failed' && s.has_saved) {
      el.label.textContent = 'Đang thử lại mạng đã lưu';
    }
    el.ssid.textContent = ssid;

    var connected = s.state === 'connected';
    el.meta.hidden = !connected;
    if (connected) {
      el.ip.textContent = s.ip || '—';
      el.rssi.textContent = rssiText(s.rssi);
      el.signal.dataset.level = rssiLevel(s.rssi);
    } else {
      delete el.signal.dataset.level;
    }

    /* Nút ngắt khi đang kết nối/đang thử lại; Kết nối lại khi đã ngắt; Quên khi có mạng đã lưu */
    var canDisconnect = connected || s.state === 'connecting' || (s.state === 'failed' && s.has_saved);
    el.btnDisc.hidden = !canDisconnect;
    el.btnReconnect.hidden = !(s.state === 'disconnected' && s.has_saved);
    el.btnForget.hidden = !s.has_saved;
    el.actions.hidden = el.btnDisc.hidden && el.btnForget.hidden && el.btnReconnect.hidden;

    if (reconnecting && connected) {
      reconnecting = false;
      showNotice('ok', 'Đã kết nối lại "' + s.ssid + '"', 'IP của hộp cơm: ' + (s.ip || '—'));
    } else if (reconnecting && s.state === 'failed') {
      reconnecting = false;
      showNotice('error', 'Chưa kết nối lại được "' + s.ssid + '"', 'Hộp cơm sẽ tiếp tục tự thử lại.');
    }

    el.ftAp.textContent = s.ap && s.ap.ssid ? 'AP: ' + s.ap.ssid : '';
    el.ftMac.textContent = s.mac ? 'MAC ' + s.mac : '';
    el.ftFw.textContent = s.device && s.device.fw ? 'FW ' + s.device.fw : '';
    renderCloud(s.cloud);
    renderOtaInfo(s.device);

    markSelected();
  }

  function refreshStatus() {
    if (uploading) return Promise.resolve(lastStatus);
    return api('GET', '/api/status', undefined, 5000).then(function (s) {
      renderStatus(s);
      if (waitingAttempt) checkAttempt(s);
      return s;
    });
  }

  function schedulePoll(ms) {
    clearTimeout(pollTimer);
    pollTimer = setTimeout(function () {
      refreshStatus().catch(function () {
        if (waitingAttempt) {
          /* AP có thể đổi kênh theo router -> điện thoại rớt mạng vài giây */
          showNotice('busy', 'Đang kết nối tới "' + waitingAttempt.ssid + '"…',
            'Điện thoại có thể bị ngắt khỏi WiFi của hộp cơm trong giây lát. Nếu trang không tự cập nhật, hãy kết nối lại WiFi của hộp.');
        }
      }).then(function () {
        if (waitingAttempt && Date.now() - waitingAttempt.started > ATTEMPT_TIMEOUT_MS) {
          finishAttempt(false, 'Quá thời gian chờ. Hãy kiểm tra lại tên và mật khẩu WiFi.');
        }
        schedulePoll(waitingAttempt ? ATTEMPT_POLL_MS : STATUS_POLL_MS);
      });
    }, ms);
  }

  /* ---------------- kết nối ---------------- */
  function checkAttempt(s) {
    var a = s.attempt || {};
    if (a.id !== waitingAttempt.id) return;
    if (a.result === 'success') {
      finishAttempt(true, 'IP của hộp cơm: ' + (s.ip || '—') + '. Bạn có thể tắt trang này.');
    } else if (a.result === 'failed') {
      finishAttempt(false, a.error || 'Không kết nối được.');
    }
  }

  function finishAttempt(ok, msg) {
    var ssid = waitingAttempt ? waitingAttempt.ssid : '';
    waitingAttempt = null;
    setBusy(el.btnConnect, false);
    if (ok) {
      showNotice('ok', 'Đã kết nối tới "' + ssid + '"', msg);
      el.inPass.value = '';
    } else {
      showNotice('error', 'Không kết nối được "' + ssid + '"', msg);
    }
  }

  el.form.addEventListener('submit', function (ev) {
    ev.preventDefault();
    var ssid = el.inSsid.value.trim();
    var pass = el.inPass.value;
    formError('');
    if (!ssid) { formError('Vui lòng chọn hoặc nhập tên WiFi.'); el.inSsid.focus(); return; }
    if (ssid.length > 32) { formError('Tên WiFi tối đa 32 ký tự.'); return; }
    if (pass.length > 0 && pass.length < 8) { formError('Mật khẩu WiFi phải có ít nhất 8 ký tự.'); el.inPass.focus(); return; }

    setBusy(el.btnConnect, true);
    showNotice('busy', 'Đang kết nối tới "' + ssid + '"…', 'Quá trình này mất khoảng 5–20 giây.');
    api('POST', '/api/connect', { ssid: ssid, password: pass }).then(function (r) {
      waitingAttempt = { id: r.attempt_id, ssid: ssid, started: Date.now() };
      schedulePoll(ATTEMPT_POLL_MS);
    }).catch(function (e) {
      setBusy(el.btnConnect, false);
      showNotice('error', 'Không gửi được yêu cầu', e.message);
    });
  });

  /* ---------------- ngắt / quên ---------------- */
  /* Bấm 2 lần để xác nhận (tránh dùng confirm() vì trình duyệt captive có thể chặn) */
  function armConfirm(btn, confirmText, action) {
    var original = btn.textContent;
    var armed = false;
    var timer = null;
    btn.addEventListener('click', function () {
      if (!armed) {
        armed = true;
        btn.textContent = confirmText;
        btn.classList.add('confirm');
        timer = setTimeout(reset, 3000);
        return;
      }
      reset();
      action(btn);
    });
    function reset() {
      armed = false;
      clearTimeout(timer);
      btn.textContent = original;
      btn.classList.remove('confirm');
    }
  }

  function doDisconnect(forget, btn) {
    btn.disabled = true;
    waitingAttempt = null;
    reconnecting = false;
    setBusy(el.btnConnect, false);
    api('POST', '/api/disconnect', { forget: forget }).then(function () {
      showNotice('ok', forget ? 'Đã xoá mạng đã lưu' : 'Đã ngắt kết nối WiFi',
        forget ? 'Hộp cơm sẽ không tự kết nối lại mạng này.' : 'Hộp cơm sẽ tự kết nối lại khi khởi động lại.');
      return refreshStatus();
    }).catch(function (e) {
      showNotice('error', 'Không thực hiện được', e.message);
    }).then(function () { btn.disabled = false; });
  }

  armConfirm(el.btnDisc, 'Bấm lại để ngắt', function (b) { doDisconnect(false, b); });
  armConfirm(el.btnForget, 'Bấm lại để quên', function (b) { doDisconnect(true, b); });

  /* Kết nối lại mạng đã lưu (không cần nhập lại mật khẩu) */
  el.btnReconnect.addEventListener('click', function () {
    var ssid = lastStatus ? lastStatus.saved_ssid : '';
    el.btnReconnect.disabled = true;
    api('POST', '/api/connect', { reconnect: true }).then(function () {
      showNotice('busy', 'Đang kết nối lại "' + ssid + '"…', '');
      reconnecting = true;
      return refreshStatus();
    }).catch(function (e) {
      showNotice('error', 'Không thực hiện được', e.message);
    }).then(function () { el.btnReconnect.disabled = false; });
  });

  /* ---------------- quét mạng ---------------- */
  var LOCK_SVG = '<svg class="net-lock" viewBox="0 0 24 24" aria-label="Có mật khẩu"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/></svg>';

  function markSelected() {
    var cur = el.inSsid.value.trim();
    var items = el.list.querySelectorAll('.net-item');
    for (var i = 0; i < items.length; i++) {
      items[i].classList.toggle('selected', items[i].dataset.ssid === cur);
    }
  }

  function renderNetworks(nets) {
    el.list.innerHTML = '';
    if (!nets.length) {
      el.list.innerHTML = '<li class="net-empty">Không tìm thấy mạng nào. Bấm quét lại.</li>';
      return;
    }
    var connectedSsid = lastStatus && lastStatus.state === 'connected' ? lastStatus.ssid : null;
    nets.forEach(function (n) {
      var li = document.createElement('li');
      var b = document.createElement('button');
      b.type = 'button';
      b.className = 'net-item';
      b.dataset.ssid = n.ssid;
      b.dataset.secure = n.secure ? '1' : '0';

      var sig = document.createElement('span');
      sig.className = 'signal';
      sig.dataset.level = rssiLevel(n.rssi);
      sig.innerHTML = '<i></i><i></i><i></i><i></i>';

      var name = document.createElement('span');
      name.className = 'net-name';
      name.textContent = n.ssid;           /* textContent: tránh chèn HTML từ SSID */

      b.appendChild(sig);
      b.appendChild(name);
      if (n.ssid === connectedSsid) {
        var tag = document.createElement('span');
        tag.className = 'net-tag';
        tag.textContent = 'Đang dùng';
        b.appendChild(tag);
      }
      if (n.secure) b.insertAdjacentHTML('beforeend', LOCK_SVG);

      b.addEventListener('click', function () {
        el.inSsid.value = n.ssid;
        el.inPass.value = '';
        formError('');
        markSelected();
        if (n.secure) {
          el.inPass.placeholder = 'Nhập mật khẩu';
          el.inPass.focus();
        } else {
          el.inPass.placeholder = 'Mạng mở, không cần mật khẩu';
          el.btnConnect.focus();
        }
      });
      li.appendChild(b);
      el.list.appendChild(li);
    });
    markSelected();
  }

  function scan() {
    if (scanning) return;
    scanning = true;
    el.btnScan.classList.add('spin');
    el.btnScan.disabled = true;
    if (!el.list.querySelector('.net-item')) {
      el.list.innerHTML = '<li class="net-empty">Đang quét mạng…</li>';
    }
    api('GET', '/api/scan', undefined, 15000).then(function (r) {
      scanRetry = 0;
      renderNetworks(r.networks || []);
    }).catch(function (e) {
      if (e.status === 503 && scanRetry < 3) {
        scanRetry++;
        setTimeout(scan, 3000);   /* thiết bị đang bận kết nối -> thử lại */
        return;
      }
      el.list.innerHTML = '';
      var li = document.createElement('li');
      li.className = 'net-empty err';
      li.textContent = e.message + '. Bạn vẫn có thể nhập tên WiFi bên dưới.';
      el.list.appendChild(li);
    }).then(function () {
      scanning = false;
      el.btnScan.classList.remove('spin');
      el.btnScan.disabled = false;
    });
  }

  el.btnScan.addEventListener('click', function () { scanRetry = 0; scan(); });
  el.inSsid.addEventListener('input', markSelected);

  el.btnShow.addEventListener('click', function () {
    var show = el.inPass.type === 'password';
    el.inPass.type = show ? 'text' : 'password';
    el.btnShow.textContent = show ? 'Ẩn' : 'Hiện';
    el.btnShow.setAttribute('aria-label', show ? 'Ẩn mật khẩu' : 'Hiện mật khẩu');
  });

  /* ---------------- máy chủ MQTT ---------------- */
  var mq = {
    form: $('mqttForm'), host: $('mqHost'), port: $('mqPort'), tls: $('mqTls'), tlsText: $('mqTlsText'),
    root: $('mqRoot'), user: $('mqUser'), pass: $('mqPass'), showPass: $('mqShowPass'),
    clearWrap: $('mqClearWrap'), clearPass: $('mqClearPass'), caBox: $('mqCaBox'), ca: $('mqCa'),
    err: $('mqErr'), save: $('mqSave'), reset: $('mqReset'), pill: $('mqPill'), status: $('mqStatus'),
    authBox: $('mqAuthBox')
  };
  var mqHasPass = false;

  /* Trạng thái từ firmware là chữ không dấu (dùng chung với OLED) -> dịch cho web */
  var CLOUD_TEXT = {
    'Broker chua chay': 'Broker từ chối kết nối (chưa chạy hoặc sai cổng)',
    'Sai ten may chu': 'Không tìm thấy tên máy chủ',
    'IP sai/tuong lua': 'Không tới được máy chủ (sai IP hoặc tường lửa)',
    'Broker k co MQTT5': 'Broker không hỗ trợ MQTT 5',
    'Sai user/mat khau': 'Sai tên đăng nhập hoặc mật khẩu',
    'Loi chung chi TLS': 'Chứng chỉ TLS không hợp lệ (kiểm tra ô CA)',
    'Loi TLS (sai cong?)': 'Bắt tay TLS thất bại (sai cổng TLS?)',
    'Cau hinh broker loi': 'Cấu hình broker không hợp lệ',
    'Mat ket noi': 'Mất kết nối, đang thử lại',
    'Dang ket noi...': 'Đang kết nối…',
    'Cho WiFi': 'Chờ WiFi'
  };

  function renderCloud(c) {
    if (!c) return;
    var wifiOk = lastStatus && lastStatus.state === 'connected';
    var kind = c.online ? 'ok' : (!wifiOk ? 'idle' : (/ket noi\.\.\.|Cho WiFi/i.test(c.status) ? 'busy' : 'error'));
    mq.pill.dataset.kind = kind;
    mq.pill.textContent = c.online ? 'Đã kết nối' : (!wifiOk ? 'Chờ WiFi' : (kind === 'busy' ? 'Đang kết nối' : 'Chưa kết nối'));
    mq.status.textContent = c.broker ? ('Đang dùng ' + c.broker + (c.online || !wifiOk ? '' : ' · ' + (CLOUD_TEXT[c.status] || c.status))) : '';
  }

  function setTlsUi() {
    mq.tlsText.textContent = mq.tls.checked ? 'Bật (mqtts)' : 'Tắt';
    mq.caBox.hidden = !mq.tls.checked;
  }

  function loadMqtt() {
    return api('GET', '/api/mqtt', undefined, 5000).then(function (m) {
      mq.host.value = m.host || '';
      mq.port.value = m.port || '';
      mq.tls.checked = !!m.tls;
      mq.root.value = m.root || '';
      mq.user.value = m.user || '';
      mq.pass.value = '';
      mqHasPass = !!m.has_pass;
      mq.pass.placeholder = mqHasPass ? 'Đã lưu, để trống nếu giữ nguyên' : 'Để trống nếu broker không yêu cầu';
      mq.clearWrap.hidden = !mqHasPass;
      mq.clearPass.checked = false;
      mq.ca.value = m.ca || '';
      if (m.user || m.has_pass) mq.authBox.open = true;
      if (m.ca) mq.caBox.open = true;
      setTlsUi();
    }).catch(function (e) {
      mq.status.textContent = 'Không đọc được cấu hình máy chủ: ' + e.message;
    });
  }

  function mqError(msg) {
    mq.err.hidden = !msg;
    mq.err.textContent = msg || '';
  }

  mq.tls.addEventListener('change', function () {
    /* đổi cổng mặc định theo TLS nếu người dùng chưa đặt cổng riêng */
    if (mq.tls.checked && (mq.port.value === '1883' || mq.port.value === '')) mq.port.value = '8883';
    if (!mq.tls.checked && mq.port.value === '8883') mq.port.value = '1883';
    setTlsUi();
  });

  mq.showPass.addEventListener('click', function () {
    var show = mq.pass.type === 'password';
    mq.pass.type = show ? 'text' : 'password';
    mq.showPass.textContent = show ? 'Ẩn' : 'Hiện';
  });

  mq.form.addEventListener('submit', function (ev) {
    ev.preventDefault();
    mqError('');
    var host = mq.host.value.trim().replace(/^(mqtts?|ssl|tcp):\/\//i, '').replace(/\/.*$/, '');
    var port = parseInt(mq.port.value, 10);
    var root = mq.root.value.trim();
    /* cho phép dán cả "host:port" vào ô địa chỉ */
    var m = host.match(/^(.+):(\d+)$/);
    if (m) { host = m[1]; port = parseInt(m[2], 10); }
    if (!host) { mqError('Vui lòng nhập địa chỉ broker.'); mq.host.focus(); return; }
    if (!/^[A-Za-z0-9._-]+$/.test(host)) { mqError('Địa chỉ broker chỉ gồm chữ, số, dấu chấm và gạch ngang.'); mq.host.focus(); return; }
    if (!(port >= 1 && port <= 65535)) { mqError('Cổng phải từ 1 đến 65535.'); mq.port.focus(); return; }
    if (!root || /[+# ]/.test(root) || /^\/|\/$/.test(root)) { mqError('Topic gốc không được trống, không chứa + # khoảng trắng, không bắt đầu/kết thúc bằng /.'); mq.root.focus(); return; }
    var ca = mq.tls.checked ? mq.ca.value.trim() : '';
    if (ca && ca.indexOf('-----BEGIN CERTIFICATE-----') < 0) { mqError('Chứng chỉ CA phải ở dạng PEM (bắt đầu bằng -----BEGIN CERTIFICATE-----).'); return; }

    var body = { host: host, port: port, tls: mq.tls.checked, root: root, user: mq.user.value.trim(), ca: ca };
    if (mq.pass.value) body.pass = mq.pass.value;
    else if (mq.clearPass.checked) body.pass = '';
    /* không gửi "pass" = giữ mật khẩu cũ */

    setBusy(mq.save, true);
    api('POST', '/api/mqtt', body, 10000).then(function () {
      showNotice('ok', 'Đã lưu cấu hình máy chủ', 'Hộp cơm đang kết nối tới ' + host + ':' + port + '. Trạng thái cập nhật ở thẻ phía trên sau vài giây.');
      return loadMqtt();
    }).catch(function (e) {
      mqError(e.message);
    }).then(function () { setBusy(mq.save, false); });
  });

  armConfirm(mq.reset, 'Bấm lại để đặt', function (b) {
    b.disabled = true;
    api('POST', '/api/mqtt', { reset: true }).then(function () {
      showNotice('ok', 'Đã về cấu hình máy chủ mặc định', 'Dùng giá trị trong app_config.h của firmware.');
      return loadMqtt();
    }).catch(function (e) { mqError(e.message); }).then(function () { b.disabled = false; });
  });

  /* ---------------- cập nhật firmware ---------------- */
  var ota = {
    info: $('otaInfo'), pick: $('otaPick'), file: $('otaFile'), name: $('otaName'),
    bar: $('otaBar'), fill: $('otaFill'), pct: $('otaPct'), err: $('otaErr'), start: $('otaStart')
  };
  var otaSupported = true;
  var fwBefore = null;

  function renderOtaInfo(d) {
    if (!d || uploading) return;
    otaSupported = d.ota !== false;
    ota.info.textContent = 'Phiên bản đang chạy: ' + (d.fw || '—') + (d.partition ? ' · phân vùng ' + d.partition : '') +
      (otaSupported ? '' : ' · Thiết bị chưa có phân vùng OTA, cần nạp qua cáp USB một lần.');
    updateOtaButton();
  }

  function otaError(msg) {
    ota.err.hidden = !msg;
    ota.err.textContent = msg || '';
  }

  function fmtSize(n) { return n >= 1048576 ? (n / 1048576).toFixed(2) + ' MB' : Math.round(n / 1024) + ' KB'; }

  function updateOtaButton() {
    var f = ota.file.files && ota.file.files[0];
    ota.start.disabled = uploading || !f || !otaSupported;
  }

  function setProgress(p) {
    ota.bar.hidden = false;
    ota.fill.style.width = p + '%';
    ota.pct.textContent = p + '%';
  }

  ota.file.addEventListener('change', function () {
    otaError('');
    var f = ota.file.files && ota.file.files[0];
    ota.pick.classList.toggle('has-file', !!f);
    if (!f) { ota.name.innerHTML = 'Chọn file <b>hop_com_thong_minh.bin</b>'; updateOtaButton(); return; }
    ota.name.textContent = f.name + ' · ' + fmtSize(f.size);
    if (!/\.bin$/i.test(f.name)) otaError('Hãy chọn file .bin (không phải .elf hay bootloader.bin).');
    else if (/bootloader|partition/i.test(f.name)) otaError('Đây không phải file firmware chính. Chọn hop_com_thong_minh.bin.');
    else if (f.size < 100 * 1024) otaError('File quá nhỏ để là firmware.');
    updateOtaButton();
    if (!ota.err.hidden) ota.start.disabled = true;
  });

  function waitReboot() {
    var started = Date.now();
    showNotice('busy', 'Đang khởi động lại…', 'Nếu điện thoại bị ngắt WiFi, hãy kết nối lại WiFi của hộp cơm.');
    (function poll() {
      api('GET', '/api/status', undefined, 3000).then(function (s) {
        if (s.device && s.device.uptime < 120) {
          var v = s.device.fw;
          showNotice('ok', 'Cập nhật thành công', 'Firmware đang chạy: ' + v + (fwBefore && fwBefore === v ? ' (cùng số phiên bản với bản cũ)' : '') + '.');
          renderStatus(s);
          return;
        }
        throw new Error('chưa khởi động lại');
      }).catch(function () {
        if (Date.now() - started > 90000) {
          showNotice('error', 'Chưa thấy hộp cơm trả lời', 'Kết nối lại WiFi của hộp cơm rồi tải lại trang để kiểm tra phiên bản.');
          return;
        }
        setTimeout(poll, 3000);
      });
    })();
  }

  ota.start.addEventListener('click', function () {
    var f = ota.file.files && ota.file.files[0];
    if (!f || uploading) return;
    otaError('');
    uploading = true;
    fwBefore = lastStatus && lastStatus.device ? lastStatus.device.fw : null;
    setBusy(ota.start, true);
    ota.file.disabled = true;
    setProgress(0);
    showNotice('busy', 'Đang tải firmware lên…', 'Không tắt nguồn hộp cơm và giữ trang này mở.');

    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.timeout = 180000;
    xhr.upload.onprogress = function (e) {
      if (e.lengthComputable) setProgress(Math.min(99, Math.floor(e.loaded * 100 / e.total)));
    };
    function done(ok, msg) {
      uploading = false;
      setBusy(ota.start, false);
      ota.file.disabled = false;
      updateOtaButton();
      if (ok) {
        setProgress(100);
        waitReboot();
      } else {
        otaError(msg);
        showNotice('error', 'Cập nhật thất bại', msg + ' Firmware cũ vẫn giữ nguyên.');
      }
    }
    xhr.onload = function () {
      var data = {};
      try { data = JSON.parse(xhr.responseText); } catch (e) { /* bỏ qua */ }
      if (xhr.status === 200 && data.ok) done(true);
      else done(false, data.error || ('Lỗi ' + xhr.status));
    };
    xhr.onerror = function () { done(false, 'Mất kết nối với hộp cơm khi đang tải lên.'); };
    xhr.ontimeout = function () { done(false, 'Quá thời gian tải lên.'); };
    xhr.send(f);
  });

  /* ---------------- khởi động ---------------- */
  refreshStatus().catch(function (e) {
    el.label.textContent = 'Không đọc được trạng thái';
    el.card.dataset.state = 'failed';
    el.ssid.textContent = e.message;
  }).then(function () {
    schedulePoll(STATUS_POLL_MS);
    loadMqtt();
    scan();
  });
})();