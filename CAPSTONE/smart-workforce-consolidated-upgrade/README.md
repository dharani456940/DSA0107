# Smart Workforce — consolidated local upgrade

This package extends the supplied React/Vite + C++20/Drogon + SQLite project. It preserves the existing login/session model and uses the existing SQLite database file at `backend/data/workforce.sqlite`.

## Features in this package
- Existing role-based login, attendance, task assignment, task status and server-side task timer.
- Employee leave records: full-day or half-day; paid or unpaid. In this demo version employee leave entries are automatically recorded as approved (there is no approval workflow).
- Productivity page with completed task count, difficulty-weighted points and tracked task-session time.
- Payroll preview includes recorded attendance time, unpaid leave deduction, overtime at 1.5x, and existing payroll adjustments.
- Payroll adjustment increases are prevented from exceeding the employee's fixed monthly salary baseline.

## Demo assumptions and important limitations
- Standard shift assumption: 09:00–18:00 with a 60-minute unpaid break; 8 paid hours/day.
- Payroll preview currently uses a simplified 26-workday monthly denominator and monthly aggregate attendance. It is a demonstration estimate, not a production payroll calculation. It does not yet implement a true per-day schedule/holiday calendar, configurable shifts, jurisdiction-specific statutory deductions, tax, benefits, or payroll-period close.
- Overtime is estimated from monthly hours above 208 and paid at 1.5x. A production system should calculate overtime per scheduled workday and apply local labor rules.
- Task session time is a productivity measure. Concurrent task timers can overlap and are not added to attendance-paid hours.
- Task difficulty points are workload indicators, not a complete performance score. Quality review, role-normalized comparisons, blockers and fair incentive rules remain future work. This package does not automatically award performance incentives.
- Paid leave records are not deducted; unpaid leave is shown separately. Leave balance accrual/limits are not yet implemented.
- The UI has been updated to show Leave and Productivity pages. Payroll remains a preview and no money is transferred.

## Build and run on Windows
1. Keep a backup of your original project and database.
2. Copy the contents of this package over the matching project files. Do not delete `backend/data/workforce.sqlite`.
3. In Developer Command Prompt for VS 2026:
   ```cmd
   cd C:\Users\Neha\smart-workforce\backend
   cmake --build build --config Release
   set WORKFORCE_DEMO=1
   build\Release\workforce_server.exe
   ```
4. In a separate PowerShell:
   ```powershell
   cd C:\Users\Neha\smart-workforce
   npm.cmd run dev
   ```
5. Open `http://localhost:5173`.

`WORKFORCE_DEMO=1` does not reset an existing database. It seeds demo accounts/tasks only when appropriate; existing database contents are preserved.

## Existing demo logins
- Manager: `manager@daywork.demo` / `ManagerDemo!42`
- Payroll admin: `payroll@daywork.demo` / `PayrollDemo!42`
- Aarav: `aarav@daywork.demo` / `AaravDemo!42`
- Diya: `diya@daywork.demo` / `DiyaDemo!42`
- Kabir: `kabir@daywork.demo` / `KabirDemo!42`
- Mira: `mira@daywork.demo` / `MiraDemo!42`
- Isha: `isha@daywork.demo` / `IshaDemo!42`
- Rohan: `rohan@daywork.demo` / `RohanDemo!42`
