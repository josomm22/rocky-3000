import { useState, useEffect, useRef, useCallback } from 'react'

/* ── Palette ────────────────────────────────────────────────────── */
const C = {
  bg:      '#111118',
  surface: '#1e1e2e',
  border:  '#25253a',
  text:    '#e8e8f0',
  muted:   '#666680',
  accent:  '#4fc3f7',
  red:     '#ef5350',
  green:   '#66bb6a',
  orange:  '#ffa726',
}

/* ── Mock data (used in dev when device is unreachable) ─────────── */
const MOCK_SHOTS = [
  {t:18.0,r:18.3},{t:18.0,r:17.6},{t:18.0,r:18.1},{t:18.5,r:18.7},
  {t:18.5,r:18.2},{t:18.0,r:18.0},{t:18.0,r:17.9},{t:18.0,r:18.4},
  {t:18.5,r:19.1},{t:18.0,r:17.8},{t:18.0,r:18.2},{t:18.0,r:18.0},
  {t:18.5,r:18.5},{t:18.0,r:17.7},{t:18.0,r:18.3},{t:18.0,r:18.1},
  {t:18.5,r:18.8},{t:18.0,r:18.0},{t:18.0,r:17.9},{t:18.0,r:18.2},
]
const MOCK_SENSOR = { live_g: 0.12, raw_g: 30210, cal: 0.000398 }

/* ── Global styles ──────────────────────────────────────────────── */
const globalCss = `
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: ${C.bg}; color: ${C.text}; font-family: system-ui, sans-serif;
         min-height: 100vh; }
  button { touch-action: manipulation; -webkit-tap-highlight-color: transparent; }
  ::-webkit-scrollbar { width: 6px; }
  ::-webkit-scrollbar-track { background: ${C.bg}; }
  ::-webkit-scrollbar-thumb { background: ${C.border}; border-radius: 3px; }
`

let _styleEl = null
function injectGlobalCss(css) {
  if (_styleEl) return
  _styleEl = document.createElement('style')
  _styleEl.textContent = css
  document.head.appendChild(_styleEl)
}

const TABS = ['History', 'Stats', 'Live']

/* ── Helpers ────────────────────────────────────────────────────── */
function fmt1(v) { return typeof v === 'number' ? v.toFixed(1) : '--' }
function fmtDelta(v) {
  if (typeof v !== 'number') return '--'
  return (v >= 0 ? '+' : '') + v.toFixed(2) + 'g'
}
function deltaColor(d) {
  if (typeof d !== 'number') return C.muted
  return d > 0.3 ? C.red : d < -0.3 ? C.orange : C.green
}

/* ══════════════════════════════════════════════════════════════════
 * Chart — canvas bar chart, DPR-aware and resize-responsive
 * ══════════════════════════════════════════════════════════════════ */
function ShotChart({ shots }) {
  const canvasRef = useRef(null)
  const pts = shots.slice(-30)

  useEffect(() => {
    const cv = canvasRef.current
    if (!cv || pts.length === 0) return

    function draw() {
      const dpr = window.devicePixelRatio || 1
      const W   = cv.offsetWidth
      const H   = 180
      cv.width  = W * dpr
      cv.height = H * dpr
      cv.style.height = H + 'px'
      const ctx = cv.getContext('2d')
      ctx.scale(dpr, dpr)

      let maxG = 0
      pts.forEach(s => { if (s.t > maxG) maxG = s.t; if (s.r > maxG) maxG = s.r })
      maxG = Math.ceil(maxG * 1.15 / 5) * 5 || 30

      const pad  = 28   // left margin for y-axis labels
      const grpW = Math.floor((W - pad) / pts.length)
      const barW = Math.max(2, Math.floor(grpW / 2) - 2)

      /* grid + y-axis labels */
      ctx.strokeStyle = C.border
      ctx.lineWidth   = 1
      ctx.fillStyle   = '#444466'
      ctx.font        = '10px system-ui'
      ctx.textAlign   = 'right'
      for (let g = 5; g <= maxG; g += 5) {
        const gy = H - (g / maxG) * H
        ctx.beginPath(); ctx.moveTo(pad, gy); ctx.lineTo(W, gy); ctx.stroke()
        ctx.fillText(g + 'g', pad - 4, gy + 3)
      }

      /* bars */
      pts.forEach((s, i) => {
        const x  = pad + i * grpW
        const th = Math.round((s.t / maxG) * H)
        const rh = Math.round((s.r / maxG) * H)
        const d  = s.r - s.t
        ctx.fillStyle = '#2e2e48'
        ctx.fillRect(x, H - th, barW, th)
        ctx.fillStyle = d > 0.5 ? C.red : d < -0.5 ? C.orange : C.accent
        ctx.fillRect(x + barW + 2, H - rh, barW, rh)
      })
    }

    draw()
    const ro = new ResizeObserver(draw)
    ro.observe(cv)
    return () => ro.disconnect()
  }, [pts])

  if (pts.length === 0) return null

  return (
    <div style={{ background: C.surface, borderRadius: 12, padding: 16, marginBottom: 16 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center',
                    marginBottom: 10 }}>
        <span style={{ fontSize: '.8rem', color: C.muted }}>Last {pts.length} shots</span>
        <div style={{ display: 'flex', gap: 12, fontSize: '.75rem', color: C.muted }}>
          <span>
            <span style={{ display: 'inline-block', width: 8, height: 8, background: '#2e2e48',
                           border: '1px solid #555', marginRight: 4, borderRadius: 2 }} />
            Target
          </span>
          <span>
            <span style={{ display: 'inline-block', width: 8, height: 8, background: C.accent,
                           marginRight: 4, borderRadius: 2 }} />
            Result
          </span>
        </div>
      </div>
      <canvas ref={canvasRef} style={{ width: '100%', display: 'block' }} />
    </div>
  )
}

/* ══════════════════════════════════════════════════════════════════
 * StatCard — min 2-per-row on narrow screens
 * ══════════════════════════════════════════════════════════════════ */
function StatCard({ label, value, color }) {
  return (
    <div style={{ background: C.surface, borderRadius: 12, padding: '14px 12px',
                  flex: '1 1 calc(50% - 6px)', textAlign: 'center' }}>
      <div style={{ fontSize: '1.5rem', fontWeight: 700, color: color || C.accent }}>
        {value}
      </div>
      <div style={{ fontSize: '.72rem', color: C.muted, marginTop: 4 }}>{label}</div>
    </div>
  )
}

/* ══════════════════════════════════════════════════════════════════
 * History tab
 * ══════════════════════════════════════════════════════════════════ */
function HistoryTab({ shots }) {
  if (shots.length === 0) {
    return <div style={{ textAlign: 'center', padding: '48px 0', color: C.muted }}>
      No grind history yet.
    </div>
  }

  const rows  = shots.slice(-20).reverse()
  const total = shots.length

  return (
    <>
      <ShotChart shots={shots} />
      {/* overflow-x: auto prevents 4-column table from clipping on narrow phones */}
      <div style={{ background: C.surface, borderRadius: 12, overflowX: 'auto',
                    WebkitOverflowScrolling: 'touch' }}>
        <table style={{ width: '100%', borderCollapse: 'collapse', minWidth: 280 }}>
          <thead>
            <tr>
              {['#', 'Target', 'Dispensed', 'Delta'].map(h => (
                <th key={h} style={{ padding: '10px 12px', textAlign: 'left',
                                     fontSize: '.72rem', color: C.muted, fontWeight: 500,
                                     borderBottom: `1px solid ${C.border}`, whiteSpace: 'nowrap' }}>
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((s, i) => {
              const d = s.r - s.t
              return (
                <tr key={i}>
                  <td style={tdStyle}>{total - i}</td>
                  <td style={tdStyle}>{fmt1(s.t)}g</td>
                  <td style={tdStyle}>{fmt1(s.r)}g</td>
                  <td style={{ ...tdStyle, color: deltaColor(d), fontWeight: 600 }}>
                    {fmtDelta(d)}
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
      </div>
    </>
  )
}
const tdStyle = { padding: '9px 12px', fontSize: '.88rem',
                  borderBottom: `1px solid #1a1a2a`, whiteSpace: 'nowrap' }

/* ══════════════════════════════════════════════════════════════════
 * Stats tab
 * ══════════════════════════════════════════════════════════════════ */
function StatsTab({ shots }) {
  if (shots.length === 0) {
    return <div style={{ textAlign: 'center', padding: '48px 0', color: C.muted }}>
      No data yet — complete some grinds first.
    </div>
  }

  const n = shots.length
  let sumR = 0, sumD = 0, sumAbsD = 0, maxOver = -Infinity, maxUnder = Infinity
  let streakOver = 0, streakUnder = 0, curStreakO = 0, curStreakU = 0

  shots.forEach(s => {
    const d = s.r - s.t
    sumR += s.r; sumD += d; sumAbsD += Math.abs(d)
    if (d > maxOver)  maxOver  = d
    if (d < maxUnder) maxUnder = d
    if (d > 0.3)      { curStreakO++; curStreakU = 0; streakOver  = Math.max(streakOver,  curStreakO) }
    else if (d < -0.3){ curStreakU++; curStreakO = 0; streakUnder = Math.max(streakUnder, curStreakU) }
    else              { curStreakO = 0; curStreakU = 0 }
  })

  const avgR    = sumR / n
  const avgD    = sumD / n
  const avgAbsD = sumAbsD / n
  const onTarget = shots.filter(s => Math.abs(s.r - s.t) <= 0.3).length
  const pct     = Math.round((onTarget / n) * 100)

  const buckets = [0, 0, 0, 0, 0, 0]
  shots.forEach(s => {
    const d = s.r - s.t
    if      (d < -1)   buckets[0]++
    else if (d < -0.5) buckets[1]++
    else if (d < 0)    buckets[2]++
    else if (d < 0.5)  buckets[3]++
    else if (d < 1)    buckets[4]++
    else               buckets[5]++
  })
  const bucketLabels = ['<−1', '−1…−½', '−½…0', '0…+½', '+½…+1', '>+1']
  const maxBucket = Math.max(...buckets)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
        <StatCard label="Total shots"   value={n} />
        <StatCard label="Avg dispensed" value={fmt1(avgR) + 'g'} />
        <StatCard label="Avg delta"     value={fmtDelta(avgD)} color={deltaColor(avgD)} />
        <StatCard label="On-target %"   value={pct + '%'}
                  color={pct >= 80 ? C.green : pct >= 60 ? C.orange : C.red} />
      </div>

      <div style={{ background: C.surface, borderRadius: 12, padding: 16 }}>
        <div style={{ fontSize: '.8rem', color: C.muted, marginBottom: 12 }}>Precision</div>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 20px' }}>
          {[
            ['Avg |delta|',   fmt1(avgAbsD) + 'g'],
            ['Max over',      '+' + fmt1(maxOver) + 'g'],
            ['Max under',     fmt1(maxUnder) + 'g'],
            ['Longest streak',streakOver + ' over / ' + streakUnder + ' under'],
          ].map(([label, val]) => (
            <div key={label}>
              <div style={{ fontSize: '.72rem', color: C.muted }}>{label}</div>
              <div style={{ fontSize: '1rem', fontWeight: 600, marginTop: 2 }}>{val}</div>
            </div>
          ))}
        </div>
      </div>

      <div style={{ background: C.surface, borderRadius: 12, padding: 16 }}>
        <div style={{ fontSize: '.8rem', color: C.muted, marginBottom: 12 }}>
          Delta distribution (g)
        </div>
        {buckets.map((count, i) => (
          <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
            <div style={{ width: 48, fontSize: '.72rem', color: C.muted,
                          textAlign: 'right', flexShrink: 0 }}>
              {bucketLabels[i]}
            </div>
            <div style={{ flex: 1, background: C.border, borderRadius: 4, height: 16,
                          overflow: 'hidden' }}>
              <div style={{ height: '100%', borderRadius: 4,
                            width: maxBucket ? (count / maxBucket * 100) + '%' : '0%',
                            background: i < 2 ? C.red : i === 2 ? C.orange :
                                        i === 3 ? C.green : i === 4 ? C.orange : C.red,
                            transition: 'width .4s' }} />
            </div>
            <div style={{ width: 20, fontSize: '.72rem', color: C.muted, flexShrink: 0 }}>
              {count}
            </div>
          </div>
        ))}
      </div>

      {shots.length >= 5 && (
        <div style={{ background: C.surface, borderRadius: 12, padding: 16 }}>
          <div style={{ fontSize: '.8rem', color: C.muted, marginBottom: 10 }}>
            Rolling 5-shot avg delta
          </div>
          {shots.slice(-10).reduce((acc, _, i, arr) => {
            if (i < 4) return acc
            const window = arr.slice(i - 4, i + 1)
            const avg = window.reduce((s, x) => s + (x.r - x.t), 0) / 5
            acc.push({ shot: shots.length - (arr.length - 1 - i), avg })
            return acc
          }, []).map(({ shot, avg }) => (
            <div key={shot} style={{ display: 'flex', justifyContent: 'space-between',
                                     padding: '5px 0', fontSize: '.88rem',
                                     borderBottom: `1px solid ${C.border}` }}>
              <span style={{ color: C.muted }}>After shot {shot}</span>
              <span style={{ fontWeight: 600, color: deltaColor(avg) }}>{fmtDelta(avg)}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}

/* ══════════════════════════════════════════════════════════════════
 * Live tab
 * ══════════════════════════════════════════════════════════════════ */
function LiveTab() {
  const [sensor, setSensor] = useState(null)
  const [error,  setError]  = useState(false)

  useEffect(() => {
    let alive = true
    async function poll() {
      try {
        const r = await fetch('/api/sensor')
        if (!r.ok) throw new Error()
        const d = await r.json()
        if (alive) { setSensor(d); setError(false) }
      } catch {
        if (alive) {
          setError(true)
          if (import.meta.env.DEV) setSensor(MOCK_SENSOR)
        }
      }
      if (alive) setTimeout(poll, 500)
    }
    poll()
    return () => { alive = false }
  }, [])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16 }}>
      <div style={{ background: C.surface, borderRadius: 16, padding: '32px 24px',
                    textAlign: 'center' }}>
        <div style={{ fontSize: '.8rem', color: C.muted, marginBottom: 8 }}>
          Live weight (calibrated)
        </div>
        <div style={{ fontSize: 'clamp(2.5rem, 12vw, 4rem)', fontWeight: 700,
                      color: C.accent, letterSpacing: '-1px', lineHeight: 1 }}>
          {sensor ? sensor.live_g.toFixed(2) : '--'}
          <span style={{ fontSize: '1.4rem', color: C.muted, fontWeight: 400 }}>g</span>
        </div>
        {error && !import.meta.env.DEV && (
          <div style={{ marginTop: 12, fontSize: '.8rem', color: C.red }}>
            ⚠ No response from device
          </div>
        )}
        {error && import.meta.env.DEV && (
          <div style={{ marginTop: 12, fontSize: '.8rem', color: C.orange }}>
            Dev mode — showing mock data
          </div>
        )}
      </div>

      <div style={{ display: 'flex', gap: 8 }}>
        <StatCard label="Raw (cal=1)"  value={sensor ? sensor.raw_g.toFixed(0) : '--'} />
        <StatCard label="Cal factor"   value={sensor ? sensor.cal.toFixed(4) : '--'} />
      </div>

      <div style={{ background: C.surface, borderRadius: 12, padding: 16,
                    fontSize: '.85rem', color: C.muted, lineHeight: 1.6 }}>
        <div style={{ color: C.text, fontWeight: 600, marginBottom: 6 }}>
          How calibration works
        </div>
        Raw × cal factor = grams. Calibrate by placing a known weight on the scale and
        adjusting until the live reading matches. Auto-tune adjusts the stop offset after
        each shot to compensate for grind-in-flight.
      </div>
    </div>
  )
}

/* ══════════════════════════════════════════════════════════════════
 * Root App
 * ══════════════════════════════════════════════════════════════════ */
export default function App() {
  injectGlobalCss(globalCss)

  const [tab,     setTab]     = useState('History')
  const [shots,   setShots]   = useState([])
  const [loading, setLoading] = useState(true)

  const fetchHistory = useCallback(async () => {
    try {
      const r = await fetch('/api/history')
      if (!r.ok) throw new Error()
      const d = await r.json()
      setShots(d.shots || [])
    } catch {
      // In dev, fall back to mock data so the UI is previewable without a device
      setShots(import.meta.env.DEV ? MOCK_SHOTS : [])
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => { fetchHistory() }, [fetchHistory])

  return (
    <div style={{ maxWidth: 560, margin: '0 auto', padding: '20px 16px',
                  /* ensure full-bleed on very small screens */
                  minWidth: 0 }}>
      <div style={{ marginBottom: 20 }}>
        <h1 style={{ color: C.accent, fontSize: '1.35rem', fontWeight: 700 }}>
          ☕ GBWUI
        </h1>
        <p style={{ color: C.muted, fontSize: '.8rem', marginTop: 2 }}>
          Grind by Weight — {shots.length} shot{shots.length !== 1 ? 's' : ''} logged
          {import.meta.env.DEV && ' (dev)'}
        </p>
      </div>

      {/* Tab bar — full-width touch targets */}
      <div style={{ display: 'flex', gap: 4, background: C.surface, borderRadius: 10,
                    padding: 4, marginBottom: 20 }}>
        {TABS.map(t => (
          <button key={t} onClick={() => setTab(t)}
            style={{ flex: 1, padding: '10px 0', borderRadius: 7, border: 'none',
                     cursor: 'pointer', fontSize: '.88rem', fontWeight: 600,
                     background: tab === t ? C.accent : 'transparent',
                     color: tab === t ? '#111' : C.muted,
                     transition: 'background .15s, color .15s',
                     minHeight: 44 /* WCAG touch target */ }}>
            {t}
          </button>
        ))}
      </div>

      {tab !== 'Live' && (
        <div style={{ marginBottom: 16, display: 'flex', justifyContent: 'flex-end' }}>
          <button onClick={fetchHistory}
            style={{ background: 'transparent', border: `1px solid ${C.border}`,
                     color: C.muted, borderRadius: 8, padding: '8px 16px',
                     fontSize: '.8rem', cursor: 'pointer', minHeight: 36 }}>
            ↻ Refresh
          </button>
        </div>
      )}

      {loading ? (
        <div style={{ textAlign: 'center', padding: '48px 0', color: C.muted }}>
          Loading…
        </div>
      ) : (
        <>
          {tab === 'History' && <HistoryTab shots={shots} />}
          {tab === 'Stats'   && <StatsTab   shots={shots} />}
          {tab === 'Live'    && <LiveTab />}
        </>
      )}

      <div style={{ marginTop: 32, textAlign: 'center', fontSize: '.72rem', color: C.border }}>
        <a href="/history" style={{ color: C.border }}>legacy view</a>
        {' · '}
        <a href="/ota" style={{ color: C.border }}>firmware update</a>
      </div>
    </div>
  )
}
