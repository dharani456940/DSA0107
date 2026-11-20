import { useEffect, useMemo, useRef, useState } from 'react'
import './App.css'

const api = async (path, options = {}) => {
  const response = await fetch(path, {
    ...options,
    credentials: 'same-origin',
    headers: { ...(options.body ? { 'Content-Type': 'application/json' } : {}), ...options.headers },
  })
  const raw = response.status === 204 ? '' : await response.text()
  let body = {}
  if (raw.trim()) {
    try { body = JSON.parse(raw) }
    catch { throw new Error(`Server returned an invalid response (${response.status}). Check that the C++ backend is running the latest build.`) }
  }
  if (!response.ok) throw new Error(body.error || `The server could not complete this request (HTTP ${response.status}).`)
  return body
}

const dateText = (value, options = { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' }) => value ? new Intl.DateTimeFormat('en-IN', options).format(new Date(value)) : '—'
const timeText = (value) => value ? new Intl.DateTimeFormat('en-IN', { hour: '2-digit', minute: '2-digit' }).format(new Date(value)) : '—'
const money = (value) => new Intl.NumberFormat('en-IN', { style: 'currency', currency: 'INR', maximumFractionDigits: 0 }).format(Number(value || 0))
const initials = (name = '') => name.split(' ').map((part) => part[0]).slice(0, 2).join('').toUpperCase()
const currentMonth = () => { const date = new Date(); return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}` }

function App() {
  const [user, setUser] = useState(null)
  const [authReady, setAuthReady] = useState(false)
  const [page, setPage] = useState('')
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [error, setError] = useState('')
  const [notice, setNotice] = useState('')
  const [busy, setBusy] = useState(false)
  const [serverOnline, setServerOnline] = useState(null)
  const [people, setPeople] = useState([])
  const [attendance, setAttendance] = useState([])
  const [tasks, setTasks] = useState([])
  const [leaveRecords, setLeaveRecords] = useState([])
  const [payroll, setPayroll] = useState(null)
  const [selectedEmployee, setSelectedEmployee] = useState('')
  const [payMonth, setPayMonth] = useState(currentMonth())
  const [cameraOpen, setCameraOpen] = useState(false)
  const [cameraError, setCameraError] = useState('')
  const [stream, setStream] = useState(null)
  const [clock, setClock] = useState(new Date())
  const [passwordForm, setPasswordForm] = useState({ currentPassword: '', newPassword: '' })
  const videoRef = useRef(null)
  const role = user?.role || ''
  const pages = role === 'manager' ? ['Overview', 'Attendance', 'Tasks', 'Productivity', 'Leave', 'People', 'Reports', 'Account']
    : role === 'payroll_admin' ? ['Overview', 'People', 'Payroll', 'Leave', 'Reports', 'Account']
      : ['My attendance', 'My tasks', 'Productivity', 'Leave', 'My payroll', 'Account']

  useEffect(() => {
    const ticker = window.setInterval(() => setClock(new Date()), 1000)
    const health = () => api('/api/health').then(() => setServerOnline(true)).catch(() => setServerOnline(false))
    health()
    const healthTimer = window.setInterval(health, 10000)
    api('/api/me').then((result) => {
      setUser(result.user)
      setPage(result.user.mustChangePassword ? 'Account' : initialPage(result.user.role))
    }).catch(() => {}).finally(() => setAuthReady(true))
    return () => { window.clearInterval(ticker); window.clearInterval(healthTimer) }
  }, [])

  useEffect(() => {
    if (!user) return
    let cancelled = false
    const load = async () => {
      const jobs = [api('/api/attendance')]
      if (user.role === 'manager' || user.role === 'employee') jobs.push(api('/api/tasks'))
      if (user.role !== 'employee') jobs.push(api('/api/people'))
      const shouldLoadPayroll = user.role === 'employee' || (user.role === 'payroll_admin' && Boolean(selectedEmployee))
      if (shouldLoadPayroll) {
        const who = user.role === 'payroll_admin' && selectedEmployee ? `?employeeId=${encodeURIComponent(selectedEmployee)}&month=${payMonth}` : `?month=${payMonth}`
        jobs.push(api(`/api/payroll${who}`))
      }
      const results = await Promise.allSettled(jobs)
      setServerOnline(results[0].status === 'fulfilled')
      if (cancelled) return
      if (results[0].status === 'fulfilled') setAttendance(results[0].value.records)
      else setError(results[0].reason.message)
      let next = 1
      if (user.role === 'manager' || user.role === 'employee') {
        if (results[next].status === 'fulfilled') setTasks(results[next].value.tasks)
        else setError(results[next].reason.message)
        next++
      }
      if (user.role !== 'employee') {
        if (results[next].status === 'fulfilled') {
          const records = results[next].value.people
          setPeople(records)
          if (user.role === 'payroll_admin' && !selectedEmployee && records.length) setSelectedEmployee(String(records[0].id))
        }
        next++
      }
      if (shouldLoadPayroll) {
        if (results[next].status === 'fulfilled') setPayroll(results[next].value)
        else setError(results[next].reason.message)
      }
    }
    load().catch((cause) => setError(cause.message))
    const refreshTimer = window.setInterval(() => load().catch((cause) => setError(cause.message)), 10000)
    return () => { cancelled = true; window.clearInterval(refreshTimer) }
  }, [user, selectedEmployee, payMonth])

  useEffect(() => {
    if (!user) return
    let cancelled = false
    const loadLeave = () => api('/api/leave').then((result) => { if (!cancelled) setLeaveRecords(result.requests || []) }).catch((cause) => { if (!cancelled) setError(cause.message) })
    loadLeave()
    const timer = window.setInterval(loadLeave, 15000)
    return () => { cancelled = true; window.clearInterval(timer) }
  }, [user])

  useEffect(() => {
    if (videoRef.current && stream) videoRef.current.srcObject = stream
  }, [stream, cameraOpen])

  const currentAttendance = useMemo(() => attendance.find((record) => (!record.user_id || Number(record.user_id) === Number(user?.id)) && !record.clock_out), [attendance, user])

  function initialPage(currentRole) {
    return currentRole === 'employee' ? 'My attendance' : currentRole === 'payroll_admin' ? 'Payroll' : 'Overview'
  }

  async function signIn(event) {
    event.preventDefault()
    setBusy(true); setError('')
    try {
      const result = await api('/api/login', { method: 'POST', body: JSON.stringify({ email, password }) })
      const account = { ...result.user, mustChangePassword: result.user.must_change_password }
      setUser(account)
      setPage(account.mustChangePassword ? 'Account' : initialPage(account.role))
      setPassword('')
    } catch (cause) { setError(cause.message) }
    finally { setBusy(false) }
  }

  async function signOut() {
    try { await api('/api/logout', { method: 'POST' }) } catch { /* clear the visible session even if the service is offline */ }
    stopCamera(); setUser(null); setPage(''); setPayroll(null); setAttendance([]); setPeople([]); setTasks([]); setNotice('')
  }

  function stopCamera() {
    if (stream) stream.getTracks().forEach((track) => track.stop())
    setStream(null); setCameraOpen(false)
  }

  async function startCamera() {
    setCameraError('')
    if (!navigator.mediaDevices?.getUserMedia) { setCameraError('Camera access requires HTTPS or localhost in a supported browser.'); setCameraOpen(true); return }
    try {
      const camera = await navigator.mediaDevices.getUserMedia({ video: { facingMode: { ideal: 'user' } }, audio: false })
      setStream(camera); setCameraOpen(true)
    } catch (cause) {
      setCameraOpen(true)
      setCameraError(cause.name === 'NotAllowedError' ? 'Camera permission was denied. Allow camera access in your browser settings, then try again.' : 'Could not open a camera. Check that one is connected and not in use by another app.')
    }
  }

  async function markAttendance(withCamera) {
    setBusy(true); setError('')
    try {
      if (withCamera) {
        const video = videoRef.current
        if (!video || !video.videoWidth) throw new Error('Wait for the camera preview before confirming attendance.')
        const canvas = document.createElement('canvas')
        canvas.width = video.videoWidth; canvas.height = video.videoHeight
        const context = canvas.getContext('2d')
        context.drawImage(video, 0, 0, canvas.width, canvas.height)
        context.clearRect(0, 0, canvas.width, canvas.height)
        canvas.width = 0; canvas.height = 0
      }
      const result = await api('/api/attendance/clock', { method: 'POST', body: JSON.stringify({ action: currentAttendance ? 'clock_out' : 'clock_in' }) })
      setAttendance((existing) => [result.record, ...existing.filter((row) => row.id !== result.record.id)])
      setNotice(currentAttendance ? `Clocked out at ${timeText(result.record.clock_out)}.` : `Clocked in at ${timeText(result.record.clock_in)}.`)
      if (withCamera) stopCamera()
    } catch (cause) { setError(cause.message) }
    finally { setBusy(false) }
  }

  async function changePassword(event) {
    event.preventDefault(); setBusy(true); setError('')
    try {
      await api('/api/change-password', { method: 'POST', body: JSON.stringify(passwordForm) })
      const session = await api('/api/me')
      setUser(session.user); setPasswordForm({ currentPassword: '', newPassword: '' }); setNotice('Your password was changed.')
    } catch (cause) { setError(cause.message) }
    finally { setBusy(false) }
  }

  async function createEmployee(event) {
    event.preventDefault(); setBusy(true); setError('')
    const form = new FormData(event.currentTarget)
    try {
      await api('/api/people', { method: 'POST', body: JSON.stringify(Object.fromEntries(form)) })
      event.currentTarget.reset(); setNotice('Employee account created. Ask them to change the temporary password at first sign-in.')
      const result = await api('/api/people'); setPeople(result.people)
    } catch (cause) { setError(cause.message) }
    finally { setBusy(false) }
  }

  async function createTask(event) {
    event.preventDefault(); setBusy(true); setError('')
    const form = new FormData(event.currentTarget)
    try {
      await api('/api/tasks', { method: 'POST', body: JSON.stringify({ employeeId: Number(form.get('employeeId')), title: form.get('title'), description: form.get('description'), difficulty: form.get('difficulty'), estimatedHours: Number(form.get('estimatedHours')), deadline: form.get('deadline') }) })
      event.currentTarget.reset(); setNotice('Task assigned successfully.')
      const result = await api('/api/tasks'); setTasks(result.tasks)
    } catch (cause) { setError(cause.message) }
    finally { setBusy(false) }
  }

  async function taskTimer(task, action) {
    setBusy(true); setError('')
    try {
      await api('/api/tasks/timer', { method: 'POST', body: JSON.stringify({ taskId: Number(task.id), action }) })
      const result = await api('/api/tasks'); setTasks(result.tasks)
      setNotice(action === 'start' ? `Timer started for “${task.title}”.` : `Timer stopped for “${task.title}”.`)
    } catch (cause) { setError(cause.message) }
    finally { setBusy(false) }
  }

  async function updateTask(task, status, progress) {
    setBusy(true); setError('')
    try {
      await api('/api/tasks/update', { method: 'POST', body: JSON.stringify({ taskId: task.id, status, progress }) })
      const result = await api('/api/tasks'); setTasks(result.tasks); setNotice('Task progress updated.')
    } catch (cause) { setError(cause.message) }
    finally { setBusy(false) }
  }

  async function submitLeave(event) {
    event.preventDefault(); setBusy(true); setError('')
    const form = new FormData(event.currentTarget)
    try {
      await api('/api/leave', { method: 'POST', body: JSON.stringify({ startDate: form.get('startDate'), endDate: form.get('endDate'), leaveType: form.get('leaveType'), duration: form.get('duration'), reason: form.get('reason') }) })
      event.currentTarget.reset(); setNotice('Leave record saved. Payroll will use its paid/unpaid classification.')
      const result = await api('/api/leave'); setLeaveRecords(result.requests || [])
    } catch (cause) { setError(cause.message) } finally { setBusy(false) }
  }

  async function saveAdjustment(event) {
    event.preventDefault(); setBusy(true); setError('')
    const form = new FormData(event.currentTarget)
    const amount = Number(form.get('amount'))
    try {
      await api('/api/payroll/adjustments', { method: 'POST', body: JSON.stringify({ employeeId: Number(selectedEmployee), kind: form.get('kind'), amount, reason: form.get('reason'), month: payMonth }) })
      event.currentTarget.reset(); setNotice('Payroll adjustment recorded with an audit entry.')
      const updated = await api(`/api/payroll?employeeId=${encodeURIComponent(selectedEmployee)}&month=${payMonth}`); setPayroll(updated)
    } catch (cause) { setError(cause.message) }
    finally { setBusy(false) }
  }

  async function saveBaseSalary(event) {
    event.preventDefault(); setBusy(true); setError('')
    const form = new FormData(event.currentTarget)
    try {
      await api('/api/payroll/base-salary', { method: 'POST', body: JSON.stringify({ employeeId: Number(selectedEmployee), amount: Number(form.get('amount')), reason: form.get('reason'), month: payMonth }) })
      setNotice('Baseline salary updated and recorded in the audit log.')
      const updated = await api(`/api/payroll?employeeId=${encodeURIComponent(selectedEmployee)}&month=${payMonth}`); setPayroll(updated)
      const list = await api('/api/people'); setPeople(list.people)
    } catch (cause) { setError(cause.message) }
    finally { setBusy(false) }
  }

  function exportAttendance() {
    const rows = [['Date (server UTC)', 'Employee', 'Clock in (UTC)', 'Clock out (UTC)'], ...attendance.map((entry) => [entry.work_date, entry.name || user.name, entry.clock_in, entry.clock_out || 'In progress'])]
    const csv = rows.map((row) => row.map((item) => `"${String(item ?? '').replaceAll('"', '""')}"`).join(',')).join('\n')
    const link = document.createElement('a'); link.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv' })); link.download = 'attendance-report.csv'; link.click(); URL.revokeObjectURL(link.href)
  }

  if (!authReady) return <main className="login-screen"><section className="login-card"><span className="brand-mark">d</span><p>Connecting to the C++ workforce service…</p></section></main>
  if (!user) return <main className="login-screen"><section className="login-card"><a className="brand login-brand" href="#login"><span className="brand-mark">d</span><span>daywork<span className="brand-dot">.</span></span></a><div className="login-label">SMART WORKFORCE</div><h1>Welcome back</h1><p className="login-subtitle">Sign in with your own account to continue.</p>{serverOnline === false && <div className="api-error" role="status">C++ server is offline. Start the backend from the commands in backend/README.md.</div>}<form className="login-form" onSubmit={signIn}><label>Email address<input type="email" autoComplete="username" value={email} onChange={(event) => setEmail(event.target.value)} required /></label><label>Password<input type="password" autoComplete="current-password" value={password} onChange={(event) => setPassword(event.target.value)} required /></label>{error && <div className="login-error" role="alert">{error}</div>}<button className="primary-button login-submit" disabled={busy || serverOnline === false}>{busy ? 'Signing in…' : 'Sign in'}</button></form>{import.meta.env.DEV && <div className="demo-credentials"><strong>Local demo accounts</strong><span>Manager: manager@daywork.demo · ManagerDemo!42</span><span>Payroll: payroll@daywork.demo · PayrollDemo!42</span><span>Employee: aarav@daywork.demo · AaravDemo!42</span><span>More accounts in backend/README.md</span></div>}<p className="security-note">Accounts and permissions are checked by the C++ server. Sample accounts are fictional and for local development only.</p></section></main>

  const visiblePage = user.mustChangePassword ? 'Account' : pages.includes(page) ? page : pages[0]
  const selected = people.find((person) => String(person.id) === String(selectedEmployee))
  const displayPayroll = payroll && selectedEmployee && String(payroll.employee.id) !== String(selectedEmployee) ? null : payroll
  const onShift = attendance.some((record) => !record.clock_out)

  return <div className="app-shell"><aside className="sidebar"><a className="brand" href="#home" onClick={() => setPage(pages[0])}><span className="brand-mark">d</span><span>daywork<span className="brand-dot">.</span></span></a><div className="workspace-label">WORKSPACE</div><div className="workspace-switch"><span className="workspace-icon">N</span><span><strong>Northstar Studio</strong><small>Connected C++ server</small></span><span className="server-led" /></div><nav className="main-nav" aria-label="Main navigation"><div className="nav-label">YOUR WORKSPACE</div>{pages.filter((item) => item !== 'Account').map((item, index) => <button key={item} className={`nav-item ${visiblePage === item ? 'selected' : ''}`} onClick={() => { setPage(item); setError(''); setNotice('') }}><span className="nav-icon">{item.includes('task') || item === 'Tasks' ? '✓' : ['◫', '◷', '♙', '▤', '₹'][index]}</span>{item}</button>)}</nav><div className="sidebar-bottom"><div className="help-card"><div className="help-spark">✳</div><strong>Live workspace</strong><p>Records are saved by the C++ server.</p></div><button className="profile-button" onClick={() => setPage('Account')}><span className="avatar avatar-lavender">{initials(user.name)}</span><span><strong>{user.name}</strong><small>{user.role.replace('_', ' ')}</small></span><span className="chevron">⌄</span></button><button className="signout-sidebar" onClick={signOut}>⇥ <span>Sign out</span></button></div></aside>
    <main className="main-content"><header className="topbar"><div className="breadcrumbs">Workspace <span>/</span> <strong>{visiblePage}</strong></div><div className="top-actions"><span className={`live-badge ${serverOnline === false ? 'offline' : ''}`}><i /> {serverOnline === false ? 'OFFLINE' : serverOnline ? 'LIVE' : 'CONNECTING'}</span><span className="today-date">{dateText(clock)}</span><span className="live-clock">{clock.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}</span></div></header><div className="page-content">
      {notice && <div className="notice" role="status"><span>✓</span>{notice}<button aria-label="Dismiss notification" onClick={() => setNotice('')}>×</button></div>}{error && <div className="api-error" role="alert"><span>{error}</span><button aria-label="Dismiss error" onClick={() => setError('')}>×</button></div>}

      {(visiblePage === 'Overview') && <><PageHeading title={`Good day, ${user.name.split(' ')[0]}`} subtitle="Live workforce data from your C++ server." /><div className="live-summary"><div><span>LOCAL CLOCK</span><strong>{clock.toLocaleTimeString()}</strong><small>Browser local time · attendance timestamps use server UTC</small></div><div><span>TEAM PRESENT</span><strong>{attendance.filter((record) => !record.clock_out).length}</strong><small>Active clock-ins in the server records</small></div><div><span>WORKSPACE</span><strong>Online <i className="online-dot" /></strong><small>Connected to SQLite-backed API</small></div></div><div className="overview-actions"><button className="primary-button" onClick={() => setPage('Attendance')}>Open attendance →</button>{role === 'payroll_admin' && <button className="secondary-button" onClick={() => setPage('Payroll')}>Review payroll →</button>}</div><section className="panel"><div className="panel-header"><div><h2>Recent attendance</h2><p>Server-recorded employee clock events.</p></div><button className="text-link" onClick={() => setPage('Attendance')}>View all →</button></div><AttendanceTable records={attendance.slice(0, 6)} currentUser={user} /></section></>}

      {(visiblePage === 'Attendance' || visiblePage === 'My attendance') && <><PageHeading title={visiblePage === 'Attendance' ? 'Team attendance' : 'My attendance'} subtitle="Clock in or out in one step. Your account can only change its own attendance." /><section className="attendance-page-card"><div><div className="status-pill"><span className="status-dot" />{currentAttendance ? `SHIFT IN PROGRESS · STARTED ${timeText(currentAttendance.clock_in)}` : 'NOT CLOCKED IN'}</div><h2>{currentAttendance ? 'You are currently clocked in.' : 'Ready to start your shift?'}</h2><p>The C++ server timestamps each action. Camera frames are not saved.</p></div><button className={`clock-button ${currentAttendance ? 'clocked' : ''}`} onClick={startCamera} disabled={busy}>{currentAttendance ? 'Camera clock-out' : 'Camera clock-in'}</button></section><div className="camera-fallback"><span>Can’t use your camera?</span><button className="text-link" disabled={busy} onClick={() => markAttendance(false)}>{currentAttendance ? 'Clock out manually' : 'Clock in manually'}</button></div><section className="panel data-panel"><div className="panel-header"><div><h2>{role === 'employee' ? 'Your recent attendance' : 'Attendance records'}</h2><p>Times are returned by the server in UTC and displayed in your local time.</p></div>{role !== 'employee' && <button className="secondary-button" onClick={exportAttendance}>Export CSV</button>}</div><AttendanceTable records={attendance} currentUser={user} /></section></>}

      {(visiblePage === 'Tasks' || visiblePage === 'My tasks') && <><PageHeading title={visiblePage === 'My tasks' ? 'My assigned tasks' : 'Task assignment'} subtitle={visiblePage === 'My tasks' ? 'View your work, update progress and track deadlines.' : 'Assign work with clear difficulty, estimated effort and deadlines.'} />{role === 'manager' && <form className="inline-form task-assignment-form" onSubmit={createTask}><h2>Assign a new task</h2><select name="employeeId" required defaultValue=""><option value="" disabled>Choose employee</option>{people.map((person) => <option value={person.id} key={person.id}>{person.name} · {person.department}</option>)}</select><input name="title" placeholder="Task title" maxLength="160" required /><input name="description" placeholder="Task description" maxLength="2000" /><select name="difficulty" defaultValue="medium"><option value="easy">Easy · 1 point</option><option value="medium">Medium · 2 points</option><option value="hard">Hard · 3 points</option><option value="expert">Expert · 4 points</option></select><input name="estimatedHours" type="number" min="0.25" max="1000" step="0.25" placeholder="Estimated hours" required /><input name="deadline" type="date" required /><button className="primary-button" disabled={busy || !people.length}>Assign task</button></form>}<div className="task-summary-grid"><div><span>TOTAL TASKS</span><strong>{tasks.length}</strong></div><div><span>IN PROGRESS</span><strong>{tasks.filter((task) => task.status === 'in_progress').length}</strong></div><div><span>COMPLETED WEIGHTED POINTS</span><strong>{tasks.filter((task) => task.status === 'completed').reduce((sum, task) => sum + ({ easy: 1, medium: 2, hard: 3, expert: 4 }[task.difficulty] || 1), 0)}</strong></div></div><section className="panel data-panel"><div className="panel-header"><div><h2>{role === 'manager' ? 'Team task board' : 'Your tasks'}</h2><p>Difficulty points: Easy 1 · Medium 2 · Hard 3 · Expert 4. Points help compare workload, not a standalone performance or pay decision.</p></div></div>{tasks.length ? <div className="task-board">{tasks.map((task) => <article className="task-card" key={task.id}><div className="task-card-top"><span className={`difficulty-pill difficulty-${task.difficulty}`}>{task.difficulty}</span><span className={`task-status status-${task.status}`}>{task.status.replace('_', ' ')}</span></div><h3>{task.title}</h3><p>{task.description || 'No additional description provided.'}</p><div className="task-meta"><span>{role === 'manager' ? task.employee_name : 'Assigned to you'}</span><span>{task.estimated_hours} estimated hrs</span><span>Due {task.deadline}</span></div>{role === 'employee' && task.status !== 'completed' && <div className="task-timer-row"><span><strong>{Math.floor(Number(task.tracked_seconds || 0) / 3600)}h {Math.floor((Number(task.tracked_seconds || 0) % 3600) / 60)}m</strong> tracked on this task</span><div className="task-controls">{task.active_started_at ? <button className="secondary-button" disabled={busy} onClick={() => taskTimer(task, 'end')}>End task</button> : <button className="primary-button" disabled={busy || !currentAttendance} onClick={() => taskTimer(task, 'start')}>Start task</button>}</div></div>}{role === 'employee' && task.status === 'completed' && <div className="task-completed-note"><span>✓</span> Completed · {Math.floor(Number(task.tracked_seconds || 0) / 3600)}h {Math.floor((Number(task.tracked_seconds || 0) % 3600) / 60)}m tracked</div>}{role === 'employee' && !currentAttendance && <small className="payroll-note">Clock in before starting a task timer.</small>}<div className="task-weight-row"><span>Difficulty weight</span><strong>{{ easy: 1, medium: 2, hard: 3, expert: 4 }[task.difficulty]} points</strong></div><div className="task-progress-track"><span style={{ width: `${task.progress}%` }} /></div><div className="task-card-bottom"><span>{task.progress}% complete</span>{role === 'employee' && <div className="task-controls"><select aria-label={`Status for ${task.title}`} value={task.status} disabled={busy} onChange={(event) => updateTask(task, event.target.value, event.target.value === 'completed' ? 100 : event.target.value === 'in_progress' ? Math.max(10, task.progress) : 0)}><option value="pending">Pending</option><option value="in_progress">In progress</option><option value="completed" disabled={Boolean(task.active_started_at)}>Completed (end timer first)</option></select></div>}</div></article>)}</div> : <div className="empty-state">{role === 'manager' ? 'No tasks assigned yet. Use the form above to assign the first task.' : 'You do not have any assigned tasks yet.'}</div>}</section></>}

      {visiblePage === 'Leave' && <><PageHeading title="Leave management" subtitle="Record full-day or half-day paid and unpaid leave. Employee submissions are automatically recorded in this demo version." />{role === 'employee' && <form className="inline-form" onSubmit={submitLeave}><h2>Record leave</h2><label>Start date<input name="startDate" type="date" required /></label><label>End date<input name="endDate" type="date" required /></label><select name="duration" defaultValue="full_day"><option value="full_day">Full day(s)</option><option value="half_day">Half day</option></select><select name="leaveType" defaultValue="paid"><option value="paid">Paid leave</option><option value="unpaid">Unpaid leave</option></select><input name="reason" placeholder="Reason for leave" maxLength="300" required /><button className="primary-button" disabled={busy}>Save leave</button></form>}<DataTable title={role === 'employee' ? 'My leave history' : 'Employee leave records'} subtitle="Paid leave is not deducted. Unpaid leave is included in the payroll breakdown." headers={['EMPLOYEE','DATES','TYPE','DURATION','REASON','STATUS']} rows={leaveRecords.map((item) => [item.employee_name || user.name, `${item.start_date} – ${item.end_date}`, item.leave_type, item.duration.replace('_',' '), item.reason, item.status])} empty="No leave records yet." /></>}

      {visiblePage === 'Productivity' && <><PageHeading title="Daily productivity" subtitle="A transparent view of attendance, task effort and difficulty-weighted completion. Task duration is not double-counted as paid hours." /><div className="live-summary report-cards"><div><span>TASKS COMPLETED</span><strong>{tasks.filter((task) => task.status === 'completed').length}</strong><small>Completed task records</small></div><div><span>WEIGHTED POINTS</span><strong>{tasks.filter((task) => task.status === 'completed').reduce((sum, task) => sum + ({easy:1,medium:2,hard:3,expert:4}[task.difficulty] || 1),0)}</strong><small>Difficulty-weighted workload</small></div><div><span>TRACKED TASK HOURS</span><strong>{(tasks.reduce((sum, task) => sum + Number(task.tracked_seconds || 0),0)/3600).toFixed(1)}</strong><small>Recorded task-session hours</small></div></div><DataTable title="Task performance detail" subtitle="Estimated versus tracked time. Speed alone is not treated as quality." headers={['TASK','DIFFICULTY','ESTIMATED HOURS','TRACKED HOURS','STATUS']} rows={tasks.map((task) => [task.title, task.difficulty, Number(task.estimated_hours).toFixed(1), (Number(task.tracked_seconds || 0)/3600).toFixed(2), task.status.replace('_',' ')])} empty="No task data available." /></>}

      {visiblePage === 'People' && <><PageHeading title="People" subtitle="Managers can create accounts. Only payroll administrators can see salary figures." />{role === 'manager' && <form className="inline-form create-employee-form" onSubmit={createEmployee}><h2>Create employee login</h2><input name="name" placeholder="Full name" required /><input name="email" type="email" placeholder="Work email" required /><input name="department" placeholder="Department" required /><input name="title" placeholder="Job title" required /><input name="temporaryPassword" type="password" placeholder="Temporary password (12+ characters)" minLength="12" required /><button className="primary-button" disabled={busy}>Create account</button><p>New employees are required to change their temporary password on first sign-in.</p></form>}<DataTable title="Employee directory" subtitle={`${people.length} employee accounts`} headers={['EMPLOYEE', 'DEPARTMENT', 'TITLE', ...(role === 'payroll_admin' ? ['BASE MONTHLY SALARY'] : [])]} rows={people.map((person) => [<span className="table-person" key={`person-${person.id}`}><i>{initials(person.name)}</i><span><strong>{person.name}</strong><small>{person.email}</small></span></span>, person.department, person.title, ...(role === 'payroll_admin' ? [money(person.base_monthly_salary)] : [])])} empty="No employees are available." /></>}

      {(visiblePage === 'Payroll' || visiblePage === 'My payroll') && <><PageHeading title={visiblePage === 'My payroll' ? 'My payroll' : 'Payroll'} subtitle="Monthly payroll preview including recorded attendance, unpaid leave and overtime. Review every component and its assumptions." />{role === 'payroll_admin' && <div className="payroll-toolbar"><label>Employee<select value={selectedEmployee} onChange={(event) => setSelectedEmployee(event.target.value)}><option value="">Select employee</option>{people.map((person) => <option value={person.id} key={person.id}>{person.name} · {person.department}</option>)}</select></label><label>Pay month<input type="month" value={payMonth} onChange={(event) => setPayMonth(event.target.value)} /></label></div>}{displayPayroll ? <><section className="payroll-total panel"><span>BASELINE MONTHLY SALARY</span><strong>{money(displayPayroll.baseSalary)}</strong><small>{displayPayroll.employee.name} · {displayPayroll.employee.title}</small><div className="baseline-note">Baseline effective {displayPayroll.baseline.effective_month} · {displayPayroll.baseline.reason} · recorded by {displayPayroll.baseline.recorded_by}</div><div className="payroll-math"><div><span>Base salary</span><strong>{money(displayPayroll.baseSalary)}</strong></div><div className="increase-line"><span>Approved increases</span><strong>+ {money(displayPayroll.increases)}</strong></div><div className="decrease-line"><span>Approved decreases</span><strong>− {money(displayPayroll.decreases)}</strong></div><div className="decrease-line"><span>Attendance shortfall (estimated)</span><strong>− {money(displayPayroll.attendanceDeduction)}</strong></div><div className="decrease-line"><span>Unpaid leave</span><strong>− {money(displayPayroll.leaveDeduction)}</strong></div><div className="increase-line"><span>Overtime ({Number(displayPayroll.overtimeHours || 0).toFixed(1)} hrs at 1.5×)</span><strong>+ {money(displayPayroll.overtimePay)}</strong></div><div className="payable-line"><span>Calculated pay for {displayPayroll.month}</span><strong>{money(displayPayroll.payable)}</strong></div></div><small className="payroll-note">Formula: {displayPayroll.calculation}. Worked hours: {Number(displayPayroll.workedHours || 0).toFixed(1)}. Shift assumption: 8 paid hours/day and 26 workdays/month. Demo estimate only; no payment is made. Tax, benefits, holidays and jurisdiction-specific payroll rules are not included.</small></section><DataTable title="Salary explanation" subtitle="Each change has a reason, period, and audit record." headers={['PERIOD', 'CHANGE', 'AMOUNT', 'REASON', 'RECORDED', 'BY']} rows={displayPayroll.changes.map((change) => [change.pay_month, change.kind === 'increase' ? 'Increase' : 'Decrease', `${change.kind === 'increase' ? '+' : '−'} ${money(change.amount)}`, change.reason, dateText(change.created_at, { day: 'numeric', month: 'short', year: 'numeric' }), change.recorded_by])} empty="No salary changes recorded for this month." />{role === 'payroll_admin' && selected && <><form className="inline-form salary-form" onSubmit={saveBaseSalary}><h2>Set baseline salary for {payMonth}</h2><input name="amount" type="number" min="0" max="100000000" placeholder={`Current baseline: ${money(selected.base_monthly_salary)}`} required /><input name="reason" placeholder="Reason for baseline change" maxLength="240" required /><button className="primary-button" disabled={busy}>Save baseline</button></form><form className="inline-form salary-form" onSubmit={saveAdjustment}><h2>Add a payroll change for {payMonth}</h2><select name="kind" defaultValue="increase"><option value="increase">Increase (+)</option><option value="decrease">Decrease (−)</option></select><input name="amount" type="number" min="1" max="100000000" placeholder="Amount in ₹" required /><input name="reason" placeholder="Required explanation" maxLength="240" required /><button className="primary-button" disabled={busy}>Record change</button></form></>}</> : <section className="panel"><p>Select an employee to load the payroll breakdown.</p></section>}</>}

      {visiblePage === 'Reports' && <><PageHeading title="Reports" subtitle="Export server attendance records for review." action={<button className="primary-button" onClick={exportAttendance}>↓ Export attendance CSV</button>} /><div className="live-summary report-cards"><div><span>RECORDS</span><strong>{attendance.length}</strong><small>Attendance entries returned by server</small></div><div><span>ACTIVE CLOCK-INS</span><strong>{attendance.filter((row) => !row.clock_out).length}</strong><small>Not yet clocked out</small></div><div><span>REPORT PERIOD</span><strong>Recent</strong><small>Latest server records</small></div></div><section className="panel data-panel"><div className="panel-header"><div><h2>Attendance report</h2><p>Use CSV export to open these records in a spreadsheet.</p></div></div><AttendanceTable records={attendance} currentUser={user} /></section></>}

      {visiblePage === 'Account' && <><PageHeading title="Account & security" subtitle={`Signed in as ${user.email}. Your role is ${role.replace('_', ' ')}.`} /><section className="panel settings-panel"><h2>Change your password</h2><p>{user.mustChangePassword ? 'Your manager created a temporary password. Change it before using the workspace.' : 'Changing your password ends other sessions for this account.'}</p><form className="password-change-form" onSubmit={changePassword}><label>Current password<input type="password" autoComplete="current-password" value={passwordForm.currentPassword} onChange={(event) => setPasswordForm({ ...passwordForm, currentPassword: event.target.value })} required /></label><label>New password (at least 12 characters)<input type="password" minLength="12" autoComplete="new-password" value={passwordForm.newPassword} onChange={(event) => setPasswordForm({ ...passwordForm, newPassword: event.target.value })} required /></label><button className="primary-button" disabled={busy}>Update password</button></form></section><section className="panel settings-panel"><h2>Session</h2><p>This account’s permissions are checked by the C++ server on every request.</p><button className="secondary-button" onClick={signOut}>Sign out</button></section></>}
      <footer className="page-footer"><span>Daywork · C++ service connected</span><span>Attendance times shown in your browser’s local timezone</span></footer>
    </div></main>
    {cameraOpen && <div className="modal-backdrop" role="presentation"><section className="camera-modal" role="dialog" aria-modal="true" aria-labelledby="camera-title"><button className="modal-close" aria-label="Close camera" onClick={stopCamera}>×</button><div className="eyebrow">CAMERA ATTENDANCE</div><h2 id="camera-title">Confirm it’s you</h2><p>Keep the camera on yourself, then confirm your {currentAttendance ? 'clock-out' : 'clock-in'}. The snapshot is discarded immediately and never uploaded.</p>{stream ? <video className="camera-video" ref={videoRef} autoPlay playsInline muted /> : <div className="camera-placeholder">Camera preview unavailable</div>}{cameraError && <div className="api-error">{cameraError}</div>}<div className="camera-actions"><button className="secondary-button" onClick={stopCamera}>Cancel</button><button className="primary-button" onClick={() => markAttendance(true)} disabled={!stream || busy}>{busy ? 'Saving…' : currentAttendance ? 'Capture & clock out' : 'Capture & clock in'}</button></div></section></div>}
  </div>
}

function initialPage(role) { return role === 'employee' ? 'My attendance' : role === 'payroll_admin' ? 'Payroll' : 'Overview' }
function PageHeading({ title, subtitle, action }) { return <div className="page-heading"><div><div className="eyebrow">WORKSPACE</div><h1>{title}</h1><p className="subtitle">{subtitle}</p></div>{action}</div> }
function AttendanceTable({ records, currentUser }) { return records.length ? <div className="data-table"><div className="data-table-head attendance-grid"><span>DATE (UTC)</span><span>EMPLOYEE</span><span>IN</span><span>OUT</span></div>{records.map((row) => <div className="data-table-row attendance-grid" key={row.id}><span>{row.work_date}</span><span>{row.name || currentUser.name}</span><span>{timeText(row.clock_in)}</span><span>{row.clock_out ? timeText(row.clock_out) : <em className="active-shift">In progress</em>}</span></div>)}</div> : <div className="empty-state">No attendance records yet. Start your first shift above.</div> }
function DataTable({ title, subtitle, headers, rows, empty }) { return <section className="panel data-panel"><div className="panel-header"><div><h2>{title}</h2><p>{subtitle}</p></div></div>{rows.length ? <div className="data-table"><div className="data-table-head" style={{ gridTemplateColumns: `repeat(${headers.length}, minmax(0, 1fr))` }}>{headers.map((header, index) => <span key={`${header}-${index}`}>{header}</span>)}</div>{rows.map((row, index) => <div className="data-table-row" key={index} style={{ gridTemplateColumns: `repeat(${headers.length}, minmax(0, 1fr))` }}>{row.map((cell, cellIndex) => <span key={cellIndex}>{cell}</span>)}</div>)}</div> : <div className="empty-state">{empty}</div>}</section> }

export default App
