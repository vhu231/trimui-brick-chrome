// Shared frame-rate probe: counts requestAnimationFrame callbacks while step() runs.
window.__probe = function (secs, step) {
  return new Promise(function (resolve) {
    var t0 = performance.now(), last = t0, ivs = [], n = 0;
    function frame(now) {
      ivs.push(now - last); last = now; n++;
      if (step) step(n);
      if (now - t0 < secs * 1000) requestAnimationFrame(frame);
      else {
        ivs.shift(); ivs.sort(function (a, b) { return a - b; });
        var q = function (p) { return Math.round(ivs[Math.floor(ivs.length * p)] * 10) / 10; };
        resolve({ fps: Math.round(n / ((now - t0) / 1000) * 10) / 10, frames: n,
                  p50: q(0.5), p95: q(0.95), over50ms: ivs.filter(function (x) { return x > 50; }).length });
      }
    }
    requestAnimationFrame(frame);
  });
};
