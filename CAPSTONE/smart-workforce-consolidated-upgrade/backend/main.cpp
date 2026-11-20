#define NOMINMAX
#include <drogon/drogon.h>
#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {

using JsonValue = ::Json::Value;
using Param = std::variant<std::int64_t, std::string>;

struct User {
    std::int64_t id{};
    std::string email;
    std::string name;
    std::string role;
    bool mustChangePassword{};
};

class Database {
public:
    Database() {
        std::filesystem::create_directories("data");
        if (sqlite3_open_v2("data/workforce.sqlite", &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
            throw std::runtime_error("Could not open the SQLite database");
        sqlite3_busy_timeout(db_, 5000);
        initialize();
    }
    ~Database() { if (db_) sqlite3_close(db_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(const std::string& sql, const std::vector<Param>& params = {}) {
        std::lock_guard lock(mutex_);
        sqlite3_stmt* statement = prepare(sql, params);
        const int result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    }

    std::vector<JsonValue> query(const std::string& sql, const std::vector<Param>& params = {}) {
        std::lock_guard lock(mutex_);
        sqlite3_stmt* statement = prepare(sql, params);
        std::vector<JsonValue> rows;
        int result = SQLITE_ROW;
        while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
            JsonValue row(Json::objectValue);
            for (int column = 0; column < sqlite3_column_count(statement); ++column) {
                const char* name = sqlite3_column_name(statement, column);
                switch (sqlite3_column_type(statement, column)) {
                    case SQLITE_INTEGER: row[name] = static_cast<Json::Int64>(sqlite3_column_int64(statement, column)); break;
                    case SQLITE_FLOAT: row[name] = sqlite3_column_double(statement, column); break;
                    case SQLITE_NULL: row[name] = JsonValue(); break;
                    default: row[name] = reinterpret_cast<const char*>(sqlite3_column_text(statement, column)); break;
                }
            }
            rows.push_back(std::move(row));
        }
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        return rows;
    }

    std::int64_t insert(const std::string& sql, const std::vector<Param>& params = {}) {
        std::lock_guard lock(mutex_);
        sqlite3_stmt* statement = prepare(sql, params);
        const int result = sqlite3_step(statement);
        const auto id = sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        return id;
    }

private:
    sqlite3* db_{};
    std::mutex mutex_;

    sqlite3_stmt* prepare(const std::string& sql, const std::vector<Param>& params) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
        for (std::size_t index = 0; index < params.size(); ++index) {
            const int position = static_cast<int>(index + 1);
            const int result = std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::string>)
                    return sqlite3_bind_text(statement, position, value.c_str(), -1, SQLITE_TRANSIENT);
                else
                    return sqlite3_bind_int64(statement, position, value);
            }, params[index]);
            if (result != SQLITE_OK) {
                sqlite3_finalize(statement);
                throw std::runtime_error(sqlite3_errmsg(db_));
            }
        }
        return statement;
    }

    void initialize() {
        const char* schema = R"SQL(
          PRAGMA foreign_keys=ON;
          CREATE TABLE IF NOT EXISTS users(
            id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, name TEXT NOT NULL,
            role TEXT NOT NULL CHECK(role IN ('employee','manager','payroll_admin')),
            password_hash TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 1,
            must_change_password INTEGER NOT NULL DEFAULT 0);
          CREATE TABLE IF NOT EXISTS employees(
            user_id INTEGER PRIMARY KEY REFERENCES users(id), department TEXT NOT NULL,
            title TEXT NOT NULL, base_monthly_salary INTEGER NOT NULL CHECK(base_monthly_salary >= 0));
          CREATE TABLE IF NOT EXISTS sessions(
            token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            expires_at INTEGER NOT NULL);
          CREATE TABLE IF NOT EXISTS attendance(
            id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
            work_date TEXT NOT NULL, clock_in TEXT NOT NULL, clock_out TEXT);
          CREATE TABLE IF NOT EXISTS tasks(
            id INTEGER PRIMARY KEY,
            assigned_to INTEGER NOT NULL REFERENCES users(id),
            created_by INTEGER NOT NULL REFERENCES users(id),
            title TEXT NOT NULL CHECK(length(title) BETWEEN 1 AND 160),
            description TEXT NOT NULL DEFAULT '',
            difficulty TEXT NOT NULL CHECK(difficulty IN ('easy','medium','hard','expert')),
            estimated_hours REAL NOT NULL CHECK(estimated_hours > 0 AND estimated_hours <= 1000000),
            deadline TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','in_progress','completed')),
            progress INTEGER NOT NULL DEFAULT 0 CHECK(progress BETWEEN 0 AND 100),
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
          CREATE INDEX IF NOT EXISTS tasks_assignee_deadline ON tasks(assigned_to, deadline);
          CREATE UNIQUE INDEX IF NOT EXISTS one_active_clock_per_user ON attendance(user_id) WHERE clock_out IS NULL;
          CREATE TABLE IF NOT EXISTS salary_changes(
            id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
            pay_month TEXT NOT NULL, kind TEXT NOT NULL CHECK(kind IN ('increase','decrease')),
            amount INTEGER NOT NULL CHECK(amount > 0), reason TEXT NOT NULL,
            created_by INTEGER NOT NULL REFERENCES users(id), created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
          CREATE TABLE IF NOT EXISTS salary_bases(
            id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
            effective_month TEXT NOT NULL, amount INTEGER NOT NULL CHECK(amount >= 0), reason TEXT NOT NULL,
            created_by INTEGER NOT NULL REFERENCES users(id), created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
          CREATE TABLE IF NOT EXISTS leave_requests(
            id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
            start_date TEXT NOT NULL, end_date TEXT NOT NULL,
            leave_type TEXT NOT NULL CHECK(leave_type IN ('paid','unpaid')),
            duration TEXT NOT NULL CHECK(duration IN ('full_day','half_day')),
            reason TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'approved',
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
          CREATE INDEX IF NOT EXISTS leave_user_dates ON leave_requests(user_id,start_date,end_date);
          CREATE TABLE IF NOT EXISTS audit_events(
            id INTEGER PRIMARY KEY, actor_id INTEGER NOT NULL REFERENCES users(id),
            action TEXT NOT NULL, subject_id INTEGER, detail TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
        )SQL";
        char* error = nullptr;
        if (sqlite3_exec(db_, schema, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : "schema initialization failed";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
        // Migrate the task-hours constraint safely. Hours are stored as milli-hours
        // (for example, 3.0 hours is stored as 3000) to avoid floating-point rounding.
        // Rebuild the table while preserving all existing task rows.
        sqlite3_exec(db_, "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr);
        const char* taskHoursMigration = R"SQL(
          BEGIN IMMEDIATE;
          CREATE TABLE tasks_rebuild(
            id INTEGER PRIMARY KEY,
            assigned_to INTEGER NOT NULL REFERENCES users(id),
            created_by INTEGER NOT NULL REFERENCES users(id),
            title TEXT NOT NULL CHECK(length(title) BETWEEN 1 AND 160),
            description TEXT NOT NULL DEFAULT '',
            difficulty TEXT NOT NULL CHECK(difficulty IN ('easy','medium','hard','expert')),
            estimated_hours REAL NOT NULL CHECK(estimated_hours > 0 AND estimated_hours <= 1000000),
            deadline TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','in_progress','completed')),
            progress INTEGER NOT NULL DEFAULT 0 CHECK(progress BETWEEN 0 AND 100),
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
          INSERT INTO tasks_rebuild(id,assigned_to,created_by,title,description,difficulty,estimated_hours,deadline,status,progress,created_at,updated_at)
            SELECT id,assigned_to,created_by,title,description,difficulty,estimated_hours,deadline,status,progress,created_at,updated_at FROM tasks;
          DROP TABLE tasks;
          ALTER TABLE tasks_rebuild RENAME TO tasks;
          CREATE INDEX IF NOT EXISTS tasks_assignee_deadline ON tasks(assigned_to, deadline);
          COMMIT;
        )SQL";
        char* migrationError = nullptr;
        if (sqlite3_exec(db_, taskHoursMigration, nullptr, nullptr, &migrationError) != SQLITE_OK) {
            const std::string message = migrationError ? migrationError : "task-hours migration failed";
            sqlite3_free(migrationError);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
            throw std::runtime_error(message);
        }
        sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);

        // Create task sessions only after the legacy task-table migration has completed.
        // This avoids a foreign-key reference blocking the migration's safe table rebuild.
        const char* taskSessionSchema = R"SQL(
          CREATE TABLE IF NOT EXISTS task_sessions(
            id INTEGER PRIMARY KEY,
            task_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
            user_id INTEGER NOT NULL REFERENCES users(id),
            started_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
            ended_at TEXT,
            duration_seconds INTEGER NOT NULL DEFAULT 0 CHECK(duration_seconds >= 0));
          CREATE INDEX IF NOT EXISTS task_sessions_task_user ON task_sessions(task_id,user_id,started_at);
          CREATE UNIQUE INDEX IF NOT EXISTS one_open_session_per_task ON task_sessions(task_id) WHERE ended_at IS NULL;
        )SQL";
        char* taskSessionError = nullptr;
        if (sqlite3_exec(db_, taskSessionSchema, nullptr, nullptr, &taskSessionError) != SQLITE_OK) {
            const std::string message = taskSessionError ? taskSessionError : "task session schema initialization failed";
            sqlite3_free(taskSessionError);
            throw std::runtime_error(message);
        }

        const auto count = query("SELECT COUNT(*) AS total FROM users");
        const bool seed = std::getenv("WORKFORCE_DEMO") && std::string(std::getenv("WORKFORCE_DEMO")) == "1";
        if (!count.empty() && count.front()["total"].asInt() == 0) {
            if (seed) seedDemoData();
            else bootstrapInitialAccounts();
        }
        // Demo additions are idempotent and never delete or reset existing user data.
        if (seed) {
            ensureAdditionalDemoEmployees();
            seedDemoTasksIfEmpty();
        }
    }

    void ensureAdditionalDemoEmployees() {
        struct Extra { const char* email; const char* name; const char* department; const char* title; int salary; const char* password; };
        const Extra extras[] = {
            {"isha@daywork.demo", "Isha Nair", "QA", "Quality analyst", 72000, "IshaDemo!42"},
            {"rohan@daywork.demo", "Rohan Das", "Data", "Data analyst", 88000, "RohanDemo!42"}
        };
        for (const auto& item : extras) {
            const auto existing = query("SELECT id FROM users WHERE email=?", {std::string(item.email)});
            if (!existing.empty()) continue;
            char passwordHash[crypto_pwhash_STRBYTES]{};
            if (crypto_pwhash_str(passwordHash, item.password, std::char_traits<char>::length(item.password),
                                  crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
                throw std::runtime_error("Could not hash an additional demo account password");
            const auto id = insert("INSERT INTO users(email,name,role,password_hash) VALUES(?,?,'employee',?)",
                {std::string(item.email), std::string(item.name), std::string(passwordHash)});
            exec("INSERT INTO employees(user_id,department,title,base_monthly_salary) VALUES(?,?,?,?)",
                 {id, std::string(item.department), std::string(item.title), static_cast<std::int64_t>(item.salary)});
            const auto manager = query("SELECT id FROM users WHERE role='manager' ORDER BY id LIMIT 1");
            if (!manager.empty()) exec("INSERT INTO salary_bases(user_id,effective_month,amount,reason,created_by) VALUES(?,strftime('%Y-%m','now','localtime'),?,'Fictional demo salary baseline',?)",
                 {id, static_cast<std::int64_t>(item.salary), manager.front()["id"].asInt64()});
        }
    }

    void seedDemoTasksIfEmpty() {
        const auto existing = query("SELECT COUNT(*) AS total FROM tasks");
        if (!existing.empty() && existing.front()["total"].asInt() > 0) return;
        const auto manager = query("SELECT id FROM users WHERE role='manager' ORDER BY id LIMIT 1");
        const auto employees = query("SELECT u.id,e.title FROM users u JOIN employees e ON e.user_id=u.id WHERE u.role='employee' AND u.active=1 ORDER BY u.id LIMIT 6");
        if (manager.empty() || employees.empty()) return;
        struct Sample { const char* title; const char* description; const char* difficulty; double hours; int days; };
        const Sample samples[] = {
            {"Update customer dashboard layout", "Improve responsive layout and verify key dashboard widgets.", "easy", 3.0, 5},
            {"Build attendance summary API", "Implement and test the monthly attendance summary endpoint.", "medium", 8.0, 7},
            {"Investigate payment reconciliation defect", "Trace the issue, document root cause and prepare a tested fix.", "hard", 12.0, 9},
            {"Design workforce analytics architecture", "Prepare an architecture proposal for fair task and payroll analytics.", "expert", 18.0, 12},
            {"Create regression test cases", "Write test cases for login, attendance and task updates.", "medium", 6.0, 6},
            {"Prepare employee onboarding guide", "Create a clear onboarding guide with screenshots and steps.", "easy", 4.0, 5},
            {"Optimize SQL reporting queries", "Review query plans and optimize the team reporting workload.", "hard", 10.0, 8},
            {"Review data quality pipeline", "Assess edge cases and propose a robust validation strategy.", "expert", 16.0, 11}
        };
        for (std::size_t i=0; i<employees.size(); ++i) {
            for (int j=0; j<3; ++j) {
                const auto& sample = samples[(i*2+j) % (sizeof(samples)/sizeof(samples[0]))];
                insert("INSERT INTO tasks(assigned_to,created_by,title,description,difficulty,estimated_hours,deadline,status,progress) VALUES(?,?,?,?,?,?,date('now',?),?,?)",
                    {employees[i]["id"].asInt64(), manager.front()["id"].asInt64(), std::string(sample.title), std::string(sample.description), std::string(sample.difficulty), static_cast<std::int64_t>(sample.hours*1000), std::string("+"+std::to_string(sample.days)+" days"), std::string(j==0?"completed":j==1?"in_progress":"pending"), static_cast<std::int64_t>(j==0?100:j==1?45:0)});
            }
        }
    }

    void seedDemoData() {
        struct Seed { const char* email; const char* name; const char* role; const char* department; const char* title; int salary; const char* password; };
        const Seed seeds[] = {
            {"manager@daywork.demo", "Riya Sen", "manager", "Operations", "Team manager", 0, "ManagerDemo!42"},
            {"payroll@daywork.demo", "Nikhil Rao", "payroll_admin", "People", "Payroll administrator", 0, "PayrollDemo!42"},
            {"aarav@daywork.demo", "Aarav Mehta", "employee", "Design", "Product designer", 85000, "AaravDemo!42"},
            {"diya@daywork.demo", "Diya Kapoor", "employee", "Engineering", "Frontend engineer", 110000, "DiyaDemo!42"},
            {"kabir@daywork.demo", "Kabir Shah", "employee", "People", "People partner", 78000, "KabirDemo!42"},
            {"mira@daywork.demo", "Mira Iyer", "employee", "Operations", "Operations lead", 95000, "MiraDemo!42"}
        };
        for (const auto& seed : seeds) {
            char passwordHash[crypto_pwhash_STRBYTES]{};
            if (crypto_pwhash_str(passwordHash, seed.password, std::char_traits<char>::length(seed.password),
                                  crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
                throw std::runtime_error("Could not hash a demo account password");
            const auto id = insert("INSERT INTO users(email,name,role,password_hash) VALUES(?,?,?,?)",
                {std::string(seed.email), std::string(seed.name), std::string(seed.role), std::string(passwordHash)});
            if (seed.salary > 0)
                exec("INSERT INTO employees(user_id,department,title,base_monthly_salary) VALUES(?,?,?,?)",
                     {id, std::string(seed.department), std::string(seed.title), static_cast<std::int64_t>(seed.salary)});
            if (seed.salary > 0)
                exec("INSERT INTO salary_bases(user_id,effective_month,amount,reason,created_by) VALUES(?,strftime('%Y-%m','now','localtime'),?,'Initial fictional demo baseline',?)",
                     {id, static_cast<std::int64_t>(seed.salary), std::int64_t{1}});
        }
    }

    void bootstrapInitialAccounts() {
        const char* managerEmail = std::getenv("WORKFORCE_BOOTSTRAP_MANAGER_EMAIL");
        const char* managerName = std::getenv("WORKFORCE_BOOTSTRAP_MANAGER_NAME");
        const char* managerPassword = std::getenv("WORKFORCE_BOOTSTRAP_MANAGER_PASSWORD");
        const char* payrollEmail = std::getenv("WORKFORCE_BOOTSTRAP_PAYROLL_EMAIL");
        const char* payrollName = std::getenv("WORKFORCE_BOOTSTRAP_PAYROLL_NAME");
        const char* payrollPassword = std::getenv("WORKFORCE_BOOTSTRAP_PAYROLL_PASSWORD");
        const bool anySet = managerEmail || managerName || managerPassword || payrollEmail || payrollName || payrollPassword;
        if (!anySet) return;
        if (!managerEmail || !managerName || !managerPassword || !payrollEmail || !payrollName || !payrollPassword ||
            std::string(managerPassword).size() < 16 || std::string(payrollPassword).size() < 16)
            throw std::runtime_error("Set all WORKFORCE_BOOTSTRAP_* values and use passwords of at least 16 characters.");
        std::string managerEmailNormalized(managerEmail), payrollEmailNormalized(payrollEmail);
        std::transform(managerEmailNormalized.begin(), managerEmailNormalized.end(), managerEmailNormalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(payrollEmailNormalized.begin(), payrollEmailNormalized.end(), payrollEmailNormalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (managerEmailNormalized == payrollEmailNormalized) throw std::runtime_error("Manager and payroll accounts must use different email addresses.");
        createInitialAccount(managerEmail, managerName, managerPassword, "manager");
        createInitialAccount(payrollEmail, payrollName, payrollPassword, "payroll_admin");
        std::cout << "Initial manager and payroll accounts created. Remove bootstrap secrets from the environment.\n";
    }

    void createInitialAccount(const char* rawEmail, const char* rawName, const char* password, const char* role) {
        std::string email(rawEmail), name(rawName);
        std::transform(email.begin(), email.end(), email.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (email.empty() || email.size() > 254 || email.find('@') == std::string::npos || name.empty() || name.size() > 120)
            throw std::runtime_error("Bootstrap account email or name is invalid.");
        char passwordHash[crypto_pwhash_STRBYTES]{};
        if (crypto_pwhash_str(passwordHash, password, std::char_traits<char>::length(password), crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
            throw std::runtime_error("Could not hash a bootstrap account password.");
        insert("INSERT INTO users(email,name,role,password_hash) VALUES(?,?,?,?)",
               {email, name, std::string(role), std::string(passwordHash)});
    }
};

Database* database{};
std::mutex loginMutex;
std::mutex attendanceMutex;
std::mutex payrollMutex;
struct LoginState { unsigned failures{}; std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now(); std::chrono::steady_clock::time_point lockedUntil{}; };
std::unordered_map<std::string, LoginState> loginStates;

bool loginBlocked(const std::string& email) {
    std::lock_guard lock(loginMutex);
    const auto found = loginStates.find(email);
    return found != loginStates.end() && std::chrono::steady_clock::now() < found->second.lockedUntil;
}

void loginFailed(const std::string& email) {
    std::lock_guard lock(loginMutex);
    auto& state = loginStates[email];
    const auto now = std::chrono::steady_clock::now();
    if (now - state.window > std::chrono::minutes(10)) state = LoginState{};
    if (++state.failures >= 6) {
        state.lockedUntil = now + std::chrono::minutes(15);
        state.failures = 0;
        state.window = now;
    }
}

void loginSucceeded(const std::string& email) {
    std::lock_guard lock(loginMutex);
    loginStates.erase(email);
}

drogon::HttpResponsePtr json(JsonValue body, drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    response->addHeader("Cache-Control", "no-store");
    return response;
}

std::string cookieValue(const drogon::HttpRequestPtr& request, const std::string& key) {
    return request->getCookie(key);
}

std::string tokenHash(const std::string& token) {
    unsigned char hash[crypto_generichash_BYTES]{};
    crypto_generichash(hash, sizeof(hash), reinterpret_cast<const unsigned char*>(token.data()), token.size(), nullptr, 0);
    char encoded[crypto_generichash_BYTES * 2 + 1]{};
    sodium_bin2hex(encoded, sizeof(encoded), hash, sizeof(hash));
    return encoded;
}

std::optional<User> currentUser(const drogon::HttpRequestPtr& request) {
    const auto token = cookieValue(request, "daywork_session");
    if (token.empty()) return std::nullopt;
    const auto rows = database->query(
        "SELECT u.id,u.email,u.name,u.role,u.must_change_password FROM sessions s JOIN users u ON u.id=s.user_id "
        "WHERE s.token_hash=? AND s.expires_at > CAST(strftime('%s','now') AS INTEGER) AND u.active=1",
        {tokenHash(token)});
    if (rows.empty()) return std::nullopt;
    return User{rows[0]["id"].asInt64(), rows[0]["email"].asString(), rows[0]["name"].asString(), rows[0]["role"].asString(), rows[0]["must_change_password"].asInt() != 0};
}

bool oneOf(const User& user, std::initializer_list<const char*> roles) {
    return std::any_of(roles.begin(), roles.end(), [&](const char* role) { return user.role == role; });
}

JsonValue error(const std::string& message) { JsonValue body; body["error"] = message; return body; }

void audit(std::int64_t actor, const std::string& action, std::optional<std::int64_t> subject, const std::string& detail) {
    database->exec("INSERT INTO audit_events(actor_id,action,subject_id,detail) VALUES(?,?,?,?)",
                   {actor, action, subject.value_or(0), detail});
}

void registerRoutes() {
    using namespace drogon;
    auto& app = drogon::app();

    app.registerHandler("/api/health", [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
        JsonValue body; body["status"] = "ok"; body["service"] = "smart-workforce-cpp";
        callback(json(std::move(body)));
    }, {Get});

    app.registerHandler("/api/login", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto input = request->getJsonObject();
        if (!input || !(*input).isMember("email") || !(*input).isMember("password")) { callback(json(error("Email and password are required."), k400BadRequest)); return; }
        auto email = (*input)["email"].asString();
        const auto password = (*input)["password"].asString();
        if (email.size() > 254 || password.size() > 1024) { callback(json(error("Invalid email or password."), k401Unauthorized)); return; }
        std::transform(email.begin(), email.end(), email.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (loginBlocked(email)) { callback(json(error("Too many sign-in attempts. Try again in 15 minutes."), k429TooManyRequests)); return; }
        const auto users = database->query("SELECT id,email,name,role,password_hash,must_change_password FROM users WHERE email=? AND active=1", {email});
        if (users.empty() || crypto_pwhash_str_verify(users[0]["password_hash"].asCString(), password.c_str(), password.size()) != 0) {
            loginFailed(email);
            callback(json(error("Invalid email or password."), k401Unauthorized)); return;
        }
        loginSucceeded(email);
        unsigned char randomToken[32]{};
        randombytes_buf(randomToken, sizeof(randomToken));
char token[sodium_base64_ENCODED_LEN(32, sodium_base64_VARIANT_URLSAFE_NO_PADDING)]{};
        sodium_bin2base64(token, sizeof(token), randomToken, sizeof(randomToken), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
        const std::string value(token);
        const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 28800;
        database->exec("INSERT INTO sessions(token_hash,user_id,expires_at) VALUES(?,?,?)", {tokenHash(value), users[0]["id"].asInt64(), expiry});
        JsonValue body; body["user"] = users[0]; body["user"].removeMember("password_hash");
        auto response = HttpResponse::newHttpJsonResponse(body);
        std::string cookie = "daywork_session=" + value + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800";
if (std::getenv("COOKIE_SECURE") && std::string(std::getenv("COOKIE_SECURE")) == "1") cookie += "; Secure";
response->addHeader("Set-Cookie", cookie);
        response->addHeader("Cache-Control", "no-store");
        callback(response);
    }, {Post});

    app.registerHandler("/api/logout", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto token = cookieValue(request, "daywork_session");
        if (!token.empty()) database->exec("DELETE FROM sessions WHERE token_hash=?", {tokenHash(token)});
        auto response = HttpResponse::newHttpJsonResponse(JsonValue(Json::objectValue));
        std::string cookie = "daywork_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
if (std::getenv("COOKIE_SECURE") && std::string(std::getenv("COOKIE_SECURE")) == "1") cookie += "; Secure";
response->addHeader("Set-Cookie", cookie);
        callback(response);
    }, {Post});

    app.registerHandler("/api/me", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        JsonValue body; body["user"]["id"] = static_cast<Json::Int64>(user->id); body["user"]["email"] = user->email; body["user"]["name"] = user->name; body["user"]["role"] = user->role; body["user"]["mustChangePassword"] = user->mustChangePassword;
        callback(json(std::move(body)));
    }, {Get});

    app.registerHandler("/api/people", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (!oneOf(*user, {"manager", "payroll_admin"})) { callback(json(error("You can only view your own employee record."), k403Forbidden)); return; }
        const bool payrollAccess = user->role == "payroll_admin";
        const auto rows = database->query(payrollAccess
            ? "SELECT u.id,u.name,u.email,u.role,e.department,e.title,COALESCE((SELECT b.amount FROM salary_bases b WHERE b.user_id=u.id AND b.effective_month<=strftime('%Y-%m','now','localtime') ORDER BY b.effective_month DESC,b.id DESC LIMIT 1),e.base_monthly_salary) AS base_monthly_salary FROM users u JOIN employees e ON e.user_id=u.id WHERE u.active=1 ORDER BY u.name"
            : "SELECT u.id,u.name,u.email,u.role,e.department,e.title FROM users u JOIN employees e ON e.user_id=u.id WHERE u.active=1 ORDER BY u.name");
        JsonValue body; body["people"] = JsonValue(Json::arrayValue); for (const auto& row : rows) body["people"].append(row);
        callback(json(std::move(body)));
    }, {Get});

    app.registerHandler("/api/people", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->mustChangePassword) { callback(json(error("Change your temporary password before managing accounts."), k403Forbidden)); return; }
        if (user->role != "manager") { callback(json(error("Only a manager can create employee accounts."), k403Forbidden)); return; }
        const auto input = request->getJsonObject();
        if (!input) { callback(json(error("Employee details are required."), k400BadRequest)); return; }
        const auto emailMember = (*input).get("email", "");
        const auto nameMember = (*input).get("name", "");
        const auto departmentMember = (*input).get("department", "");
        const auto titleMember = (*input).get("title", "");
        const auto passwordMember = (*input).get("temporaryPassword", "");
        const std::string email = emailMember.asString();
        const std::string name = nameMember.asString();
        const std::string department = departmentMember.asString();
        const std::string title = titleMember.asString();
        const std::string temporaryPassword = passwordMember.asString();
        if (email.empty() || email.size() > 254 || email.find('@') == std::string::npos || email.find_first_of(" \t\r\n") != std::string::npos || name.empty() || name.size() > 120 || department.empty() || department.size() > 80 || title.empty() || title.size() > 120 || temporaryPassword.size() < 12 || temporaryPassword.size() > 128) {
            callback(json(error("Provide a valid email, name, department, title, and a temporary password of at least 12 characters."), k400BadRequest)); return;
        }
        char passwordHash[crypto_pwhash_STRBYTES]{};
        if (crypto_pwhash_str(passwordHash, temporaryPassword.c_str(), temporaryPassword.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
            callback(json(error("Password hashing failed. Try again."), k500InternalServerError)); return;
        }
        try {
            std::string normalizedEmail = email;
            std::transform(normalizedEmail.begin(), normalizedEmail.end(), normalizedEmail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto employeeId = database->insert("INSERT INTO users(email,name,role,password_hash,must_change_password) VALUES(?,?,'employee',?,1)",
                {normalizedEmail, name, std::string(passwordHash)});
            database->exec("INSERT INTO employees(user_id,department,title,base_monthly_salary) VALUES(?,?,?,0)", {employeeId, department, title});
            audit(user->id, "employee_account_created", employeeId, normalizedEmail);
            JsonValue body; body["id"] = static_cast<Json::Int64>(employeeId); body["status"] = "created"; body["passwordChangeRequired"] = true;
            callback(json(std::move(body), k201Created));
        } catch (const std::exception&) {
            callback(json(error("Could not create account. Check that the email is not already in use."), k409Conflict));
        }
    }, {Post});

    app.registerHandler("/api/tasks", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->mustChangePassword) { callback(json(error("Change your temporary password first."), k403Forbidden)); return; }
        const auto rows = user->role == "manager"
            ? database->query("SELECT t.*,u.name AS employee_name,e.department,e.title AS employee_title,COALESCE((SELECT SUM(ts.duration_seconds) FROM task_sessions ts WHERE ts.task_id=t.id),0) AS tracked_seconds,(SELECT ts.started_at FROM task_sessions ts WHERE ts.task_id=t.id AND ts.user_id=t.assigned_to AND ts.ended_at IS NULL ORDER BY ts.id DESC LIMIT 1) AS active_started_at FROM tasks t JOIN users u ON u.id=t.assigned_to JOIN employees e ON e.user_id=u.id ORDER BY CASE t.status WHEN 'pending' THEN 0 WHEN 'in_progress' THEN 1 ELSE 2 END,t.deadline,t.id DESC")
            : database->query("SELECT t.*,u.name AS employee_name,e.department,e.title AS employee_title,COALESCE((SELECT SUM(ts.duration_seconds) FROM task_sessions ts WHERE ts.task_id=t.id),0) AS tracked_seconds,(SELECT ts.started_at FROM task_sessions ts WHERE ts.task_id=t.id AND ts.user_id=t.assigned_to AND ts.ended_at IS NULL ORDER BY ts.id DESC LIMIT 1) AS active_started_at FROM tasks t JOIN users u ON u.id=t.assigned_to JOIN employees e ON e.user_id=u.id WHERE t.assigned_to=? ORDER BY CASE t.status WHEN 'pending' THEN 0 WHEN 'in_progress' THEN 1 ELSE 2 END,t.deadline,t.id DESC", {user->id});
        JsonValue body; body["tasks"] = JsonValue(Json::arrayValue);
        for (const auto& row : rows) { auto item = row; item["estimated_hours"] = row["estimated_hours"].asDouble() / 1000.0; body["tasks"].append(item); }
        callback(json(std::move(body)));
    }, {Get});

    app.registerHandler("/api/tasks", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->mustChangePassword) { callback(json(error("Change your temporary password first."), k403Forbidden)); return; }
        if (user->role != "manager") { callback(json(error("Only managers can assign tasks."), k403Forbidden)); return; }
        const auto input = request->getJsonObject();
        if (!input) { callback(json(error("Task details are required."), k400BadRequest)); return; }
        const auto employeeId = (*input).get("employeeId", 0).asInt64();
        const auto title = (*input).get("title", "").asString();
        const auto description = (*input).get("description", "").asString();
        const auto difficulty = (*input).get("difficulty", "").asString();
        const auto estimatedHours = (*input).get("estimatedHours", 0).asDouble();
        const auto deadline = (*input).get("deadline", "").asString();
        if (employeeId <= 0 || title.empty() || title.size() > 160 || description.size() > 2000 || (difficulty != "easy" && difficulty != "medium" && difficulty != "hard" && difficulty != "expert") || estimatedHours <= 0 || estimatedHours > 1000 || deadline.size() != 10) {
            callback(json(error("Provide an employee, task title, valid difficulty, estimated hours, and deadline."), k400BadRequest)); return;
        }
        const auto employee = database->query("SELECT id FROM users WHERE id=? AND role='employee' AND active=1", {employeeId});
        if (employee.empty()) { callback(json(error("Choose an active employee account."), k400BadRequest)); return; }
        try {
            const auto taskId = database->insert("INSERT INTO tasks(assigned_to,created_by,title,description,difficulty,estimated_hours,deadline) VALUES(?,?,?,?,?,?,?)",
                {employeeId, user->id, title, description, difficulty, static_cast<std::int64_t>(estimatedHours * 1000), deadline});
            // Store estimated hours in thousandths to keep the existing database parameter binder integer-safe.
            database->exec("UPDATE tasks SET estimated_hours=? WHERE id=?", {static_cast<std::int64_t>(estimatedHours * 1000), taskId});
            audit(user->id, "task_assigned", employeeId, title + " [" + difficulty + "]");
            JsonValue body; body["id"] = static_cast<Json::Int64>(taskId); body["status"] = "created";
            callback(json(std::move(body), k201Created));
        } catch (const std::exception&) { callback(json(error("Could not save the task."), k500InternalServerError)); }
    }, {Post});

    app.registerHandler("/api/tasks/update", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->mustChangePassword) { callback(json(error("Change your temporary password first."), k403Forbidden)); return; }
        const auto input = request->getJsonObject();
        if (!input) { callback(json(error("Task update is required."), k400BadRequest)); return; }
        const auto taskId = (*input).get("taskId", 0).asInt64();
        const auto status = (*input).get("status", "").asString();
        const auto progress = (*input).get("progress", 0).asInt();
        if (taskId <= 0 || progress < 0 || progress > 100 || (status != "pending" && status != "in_progress" && status != "completed")) {
            callback(json(error("Choose a valid task status and progress from 0 to 100."), k400BadRequest)); return;
        }
        const auto task = database->query("SELECT assigned_to FROM tasks WHERE id=?", {taskId});
        if (task.empty()) { callback(json(error("Task not found."), k404NotFound)); return; }
        if (user->role != "manager" && task.front()["assigned_to"].asInt64() != user->id) { callback(json(error("You can only update your own assigned tasks."), k403Forbidden)); return; }
        if (status == "completed") {
            const auto activeTimer = database->query("SELECT id FROM task_sessions WHERE task_id=? AND user_id=? AND ended_at IS NULL LIMIT 1", {taskId, task.front()["assigned_to"].asInt64()});
            if (!activeTimer.empty()) { callback(json(error("End the task timer before marking this task completed."), k409Conflict)); return; }
        }
        const int finalProgress = status == "completed" ? 100 : progress;
        try {
            database->exec("UPDATE tasks SET status=?,progress=?,updated_at=CURRENT_TIMESTAMP WHERE id=?", {status, static_cast<std::int64_t>(finalProgress), taskId});
            audit(user->id, "task_progress_updated", task.front()["assigned_to"].asInt64(), "Task " + std::to_string(taskId) + " set to " + status + " (" + std::to_string(finalProgress) + "%)");
            callback(json(JsonValue(Json::objectValue)));
        } catch (const std::exception&) { callback(json(error("Could not update task progress."), k500InternalServerError)); }
    }, {Post});

    app.registerHandler("/api/tasks/timer", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->role != "employee") { callback(json(error("Only employees can start or end their own task timers."), k403Forbidden)); return; }
        const auto input = request->getJsonObject();
        if (!input) { callback(json(error("Task timer action is required."), k400BadRequest)); return; }
        const auto taskId = (*input).get("taskId", 0).asInt64();
        const auto action = (*input).get("action", "").asString();
        if (taskId <= 0 || (action != "start" && action != "end")) { callback(json(error("Choose a task and a valid start/end action."), k400BadRequest)); return; }
        const auto task = database->query("SELECT id,status FROM tasks WHERE id=? AND assigned_to=?", {taskId, user->id});
        if (task.empty()) { callback(json(error("This task is not assigned to your account."), k403Forbidden)); return; }
        if (action == "start") {
            if (task.front()["status"].asString() == "completed") {
                callback(json(error("This task is already completed. Reopen it before starting another work session."), k409Conflict)); return;
            }
            const auto shift = database->query("SELECT id FROM attendance WHERE user_id=? AND clock_out IS NULL ORDER BY id DESC LIMIT 1", {user->id});
            if (shift.empty()) { callback(json(error("Clock in before starting a task."), k409Conflict)); return; }
            const auto open = database->query("SELECT id FROM task_sessions WHERE task_id=? AND user_id=? AND ended_at IS NULL", {taskId, user->id});
            if (!open.empty()) { callback(json(error("This task timer is already running."), k409Conflict)); return; }
            try {
                database->insert("INSERT INTO task_sessions(task_id,user_id) VALUES(?,?)", {taskId, user->id});
                database->exec("UPDATE tasks SET status='in_progress',progress=MAX(progress,10),updated_at=CURRENT_TIMESTAMP WHERE id=?", {taskId});
                audit(user->id, "task_timer_started", user->id, "Started task " + std::to_string(taskId));
            } catch (const std::exception&) { callback(json(error("Could not start this task timer."), k500InternalServerError)); return; }
        } else {
            const auto open = database->query("SELECT id,started_at FROM task_sessions WHERE task_id=? AND user_id=? AND ended_at IS NULL ORDER BY id DESC LIMIT 1", {taskId, user->id});
            if (open.empty()) { callback(json(error("There is no running timer for this task."), k409Conflict)); return; }
            try {
                database->exec("UPDATE task_sessions SET ended_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'),duration_seconds=MAX(0,CAST((julianday('now')-julianday(started_at))*86400 AS INTEGER)) WHERE id=?", {open.front()["id"].asInt64()});
                audit(user->id, "task_timer_ended", user->id, "Ended task " + std::to_string(taskId));
            } catch (const std::exception&) { callback(json(error("Could not stop this task timer."), k500InternalServerError)); return; }
        }
        const auto totals = database->query("SELECT COALESCE(SUM(duration_seconds),0) AS tracked_seconds,(SELECT started_at FROM task_sessions WHERE task_id=? AND user_id=? AND ended_at IS NULL ORDER BY id DESC LIMIT 1) AS active_started_at FROM task_sessions WHERE task_id=? AND user_id=?", {taskId,user->id,taskId,user->id});
        JsonValue body; body["taskId"] = static_cast<Json::Int64>(taskId); body["action"] = action;
        if (!totals.empty()) { body["tracked_seconds"] = totals.front()["tracked_seconds"]; body["active_started_at"] = totals.front()["active_started_at"]; }
        callback(json(std::move(body)));
    }, {Post});

    app.registerHandler("/api/change-password", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        const auto input = request->getJsonObject();
        if (!input) { callback(json(error("Current and new passwords are required."), k400BadRequest)); return; }
        const auto oldPassword = (*input).get("currentPassword", "").asString();
        const auto newPassword = (*input).get("newPassword", "").asString();
        const auto rows = database->query("SELECT password_hash FROM users WHERE id=?", {user->id});
        if (rows.empty() || crypto_pwhash_str_verify(rows[0]["password_hash"].asCString(), oldPassword.c_str(), oldPassword.size()) != 0) {
            callback(json(error("Current password is incorrect."), k401Unauthorized)); return;
        }
        if (newPassword.size() < 12 || newPassword.size() > 128 || newPassword == oldPassword) {
            callback(json(error("Choose a different password with at least 12 characters."), k400BadRequest)); return;
        }
        char passwordHash[crypto_pwhash_STRBYTES]{};
        if (crypto_pwhash_str(passwordHash, newPassword.c_str(), newPassword.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
            callback(json(error("Password hashing failed. Try again."), k500InternalServerError)); return;
        }
        const auto currentToken = tokenHash(cookieValue(request, "daywork_session"));
        database->exec("UPDATE users SET password_hash=?,must_change_password=0 WHERE id=?", {std::string(passwordHash), user->id});
        database->exec("DELETE FROM sessions WHERE user_id=? AND token_hash<>?", {user->id, currentToken});
        audit(user->id, "password_changed", user->id, "User changed their own password");
        JsonValue body; body["status"] = "updated";
        callback(json(std::move(body)));
    }, {Post});

    app.registerHandler("/api/attendance", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->mustChangePassword) { callback(json(error("Change your temporary password first."), k403Forbidden)); return; }
        const auto rows = oneOf(*user, {"manager", "payroll_admin"})
            ? database->query("SELECT a.id,a.user_id,u.name,a.work_date,a.clock_in,a.clock_out FROM attendance a JOIN users u ON u.id=a.user_id ORDER BY a.clock_in DESC LIMIT 200")
            : database->query("SELECT id,user_id,work_date,clock_in,clock_out FROM attendance WHERE user_id=? ORDER BY clock_in DESC LIMIT 60", {user->id});
        JsonValue body; body["records"] = JsonValue(Json::arrayValue); for (const auto& row : rows) body["records"].append(row);
        callback(json(std::move(body)));
    }, {Get});

    app.registerHandler("/api/attendance/clock", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->mustChangePassword) { callback(json(error("Change your temporary password first."), k403Forbidden)); return; }
        const auto input = request->getJsonObject();
        const auto action = input && (*input).isMember("action") ? (*input)["action"].asString() : "";
        if (action != "clock_in" && action != "clock_out") { callback(json(error("Choose clock_in or clock_out."), k400BadRequest)); return; }
        std::lock_guard attendanceLock(attendanceMutex);
        const auto current = database->query("SELECT id FROM attendance WHERE user_id=? AND clock_out IS NULL ORDER BY id DESC LIMIT 1", {user->id});
        if (action == "clock_in") {
            if (!current.empty()) { callback(json(error("You are already clocked in."), k409Conflict)); return; }
            database->exec("INSERT INTO attendance(user_id,work_date,clock_in) VALUES(?,date('now','localtime'),strftime('%Y-%m-%dT%H:%M:%fZ','now'))", {user->id});
        } else {
            if (current.empty()) { callback(json(error("You are not clocked in."), k409Conflict)); return; }
            const auto activeTasks = database->query("SELECT COUNT(*) AS total FROM task_sessions WHERE user_id=? AND ended_at IS NULL", {user->id});
            if (!activeTasks.empty() && activeTasks.front()["total"].asInt() > 0) { callback(json(error("End all running task timers before clocking out."), k409Conflict)); return; }
            database->exec("UPDATE attendance SET clock_out=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=? AND user_id=? AND clock_out IS NULL", {current[0]["id"].asInt64(), user->id});
        }
        audit(user->id, action, user->id, "Employee self-service attendance action");
        const auto latest = database->query("SELECT id,user_id,work_date,clock_in,clock_out FROM attendance WHERE user_id=? ORDER BY id DESC LIMIT 1", {user->id});
        JsonValue body; body["record"] = latest.empty() ? JsonValue(Json::objectValue) : latest.front();
        callback(json(std::move(body), k201Created));
    }, {Post});

    app.registerHandler("/api/leave", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (request->method() == drogon::Get) {
            const auto rows = (user->role == "manager" || user->role == "payroll_admin")
              ? database->query("SELECT l.*,u.name AS employee_name FROM leave_requests l JOIN users u ON u.id=l.user_id ORDER BY l.start_date DESC,l.id DESC LIMIT 300")
              : database->query("SELECT * FROM leave_requests WHERE user_id=? ORDER BY start_date DESC,id DESC", {user->id});
            JsonValue body; body["requests"] = JsonValue(Json::arrayValue); for (const auto& row : rows) body["requests"].append(row);
            callback(json(std::move(body))); return;
        }
        if (user->role != "employee") { callback(json(error("Only employees can submit their own leave."), k403Forbidden)); return; }
        const auto input = request->getJsonObject();
        if (!input) { callback(json(error("Leave details are required."), k400BadRequest)); return; }
        const auto start = (*input).get("startDate", "").asString();
        const auto end = (*input).get("endDate", "").asString();
        const auto type = (*input).get("leaveType", "paid").asString();
        const auto duration = (*input).get("duration", "full_day").asString();
        const auto reason = (*input).get("reason", "").asString();
        if (start.size()!=10 || end.size()!=10 || end < start || (type!="paid" && type!="unpaid") || (duration!="full_day" && duration!="half_day") || (duration=="half_day" && start!=end) || reason.empty() || reason.size()>300) { callback(json(error("Enter valid leave dates, type, duration and reason. Half-day leave must use the same start and end date."), k400BadRequest)); return; }
        try {
          const auto id = database->insert("INSERT INTO leave_requests(user_id,start_date,end_date,leave_type,duration,reason,status) VALUES(?,?,?,?,?,?, 'approved')", {user->id,start,end,type,duration,reason});
          audit(user->id,"leave_auto_recorded",user->id,"leave="+type+" | duration="+duration+" | "+start+" to "+end);
          JsonValue body; body["id"] = static_cast<Json::Int64>(id); body["status"]="approved"; callback(json(std::move(body),k201Created));
        } catch (const std::exception&) { callback(json(error("Could not save leave record."),k500InternalServerError)); }
    }, {Get, Post});

    app.registerHandler("/api/payroll", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->mustChangePassword) { callback(json(error("Change your temporary password first."), k403Forbidden)); return; }
        std::int64_t employeeId = user->id;
        if (user->role == "payroll_admin") {
            const auto requested = request->getParameter("employeeId");
            if (!requested.empty()) {
                try { employeeId = std::stoll(requested); } catch (...) { callback(json(error("Invalid employee ID."), k400BadRequest)); return; }
            }
        }
        const auto monthParam = request->getParameter("month");
        const auto currentMonth = database->query("SELECT strftime('%Y-%m','now','localtime') AS month");
        const bool validMonth = monthParam.size() == 7 && monthParam[4] == '-' &&
            std::all_of(monthParam.begin(), monthParam.end(), [](char c) { return c == '-' || std::isdigit(static_cast<unsigned char>(c)); });
        const std::string month = validMonth ? monthParam : currentMonth.front()["month"].asString();
        const auto employees = database->query("SELECT u.id,u.name,e.department,e.title,e.base_monthly_salary FROM users u JOIN employees e ON e.user_id=u.id WHERE u.id=? AND u.active=1", {employeeId});
        if (employees.empty()) { callback(json(error("Payroll record not found."), k404NotFound)); return; }
        if (employeeId != user->id && user->role != "payroll_admin") { callback(json(error("You can only view your own payroll."), k403Forbidden)); return; }
        const auto baseline = database->query("SELECT b.amount,b.reason,b.effective_month,b.created_at,c.name AS recorded_by FROM salary_bases b JOIN users c ON c.id=b.created_by WHERE b.user_id=? AND b.effective_month<=? ORDER BY b.effective_month DESC,b.id DESC LIMIT 1", {employeeId, month});
        if (baseline.empty()) { callback(json(error("No baseline salary has been set for this employee and month."), k409Conflict)); return; }
        const auto changes = database->query("SELECT s.id,s.pay_month,s.kind,s.amount,s.reason,s.created_at,c.name AS recorded_by FROM salary_changes s JOIN users c ON c.id=s.created_by WHERE s.user_id=? AND s.pay_month=? ORDER BY s.id DESC", {employeeId, month});
        std::int64_t increase = 0, decrease = 0;
        for (const auto& change : changes) {
            if (change["kind"].asString() == "increase") increase += change["amount"].asInt64();
            else decrease += change["amount"].asInt64();
        }
        const auto attendanceCalc = database->query(
          "SELECT COALESCE(SUM(MAX(0,(julianday(COALESCE(clock_out,strftime('%Y-%m-%dT%H:%M:%fZ','now')))-julianday(clock_in))*24*60-60)),0) AS minutes "
          "FROM attendance WHERE user_id=? AND substr(work_date,1,7)=?", {employeeId,month});
        const auto leaveCalc = database->query("SELECT COALESCE(SUM(CASE WHEN duration='half_day' THEN 0.5 ELSE (julianday(end_date)-julianday(start_date)+1) END),0) AS unpaid_days, COALESCE(SUM(CASE WHEN leave_type='paid' THEN CASE WHEN duration='half_day' THEN 0.5 ELSE (julianday(end_date)-julianday(start_date)+1) END ELSE 0 END),0) AS paid_days FROM leave_requests WHERE user_id=? AND substr(start_date,1,7)=?", {employeeId,month});
        const std::int64_t base = baseline.front()["amount"].asInt64();
        const double workedHours = attendanceCalc.empty()?0.0:attendanceCalc.front()["minutes"].asDouble()/60.0;
        const double hourlyRate = base / 26.0 / 8.0;
        const double unpaidDays = leaveCalc.empty()?0.0:leaveCalc.front()["unpaid_days"].asDouble();
        const double leaveDeduction = unpaidDays * (base / 26.0);
        const double expectedHours = 26.0*8.0;
        const double shortHours = std::max(0.0, expectedHours - workedHours - unpaidDays*8.0 - (leaveCalc.empty()?0.0:leaveCalc.front()["paid_days"].asDouble()*8.0));
        const double attendanceDeduction = std::max(0.0, shortHours*hourlyRate);
        JsonValue body; body["employee"] = employees.front(); body["month"] = month;
        body["workedHours"] = workedHours; body["hourlyRate"] = hourlyRate; body["unpaidLeaveDays"] = unpaidDays;
        body["attendanceDeduction"] = static_cast<Json::Int64>(std::llround(attendanceDeduction));
        body["leaveDeduction"] = static_cast<Json::Int64>(std::llround(leaveDeduction));
        body["overtimeHours"] = std::max(0.0,workedHours-expectedHours);
        body["overtimePay"] = static_cast<Json::Int64>(std::llround(std::max(0.0,workedHours-expectedHours)*hourlyRate*1.5));
        body["baseSalary"] = base; body["baseline"] = baseline.front();
        body["increases"] = static_cast<Json::Int64>(increase); body["decreases"] = static_cast<Json::Int64>(decrease);
        body["payable"] = std::max<std::int64_t>(0, base + increase - decrease - static_cast<std::int64_t>(std::llround(attendanceDeduction)) - static_cast<std::int64_t>(std::llround(leaveDeduction)) + static_cast<std::int64_t>(std::llround(std::max(0.0,workedHours-expectedHours)*hourlyRate*1.5)));
        body["changes"] = JsonValue(Json::arrayValue); for (const auto& change : changes) body["changes"].append(change);
        body["calculation"] = "base salary + recorded changes - estimated attendance shortfall - unpaid leave + overtime (1.5x)";
        callback(json(std::move(body)));
    }, {Get});

    app.registerHandler("/api/payroll/adjustments", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->mustChangePassword) { callback(json(error("Change your temporary password first."), k403Forbidden)); return; }
        if (user->role != "payroll_admin") { callback(json(error("Payroll administrator permission required."), k403Forbidden)); return; }
        const auto input = request->getJsonObject();
        if (!input || !(*input).isMember("employeeId") || !(*input).isMember("kind") || !(*input).isMember("amount") || !(*input).isMember("reason") || !(*input).isMember("month")) {
            callback(json(error("Employee, adjustment type, amount, reason, and month are required."), k400BadRequest)); return;
        }
        std::lock_guard payrollLock(payrollMutex);
        const auto employeeId = (*input)["employeeId"].asInt64();
        const auto kind = (*input)["kind"].asString();
        const auto amount = (*input)["amount"].asInt64();
        const auto reason = (*input)["reason"].asString();
        const auto month = (*input)["month"].asString();
        const bool validMonth = month.size() == 7 && month[4] == '-' && month.substr(0, 4).find_first_not_of("0123456789") == std::string::npos && month.substr(5, 2).find_first_not_of("0123456789") == std::string::npos && std::stoi(month.substr(5, 2)) >= 1 && std::stoi(month.substr(5, 2)) <= 12;
        if ((kind != "increase" && kind != "decrease") || amount <= 0 || amount > 100000000 || reason.find_first_not_of(" \t\r\n") == std::string::npos || reason.size() > 240 || !validMonth) {
            callback(json(error("Adjustment details are invalid."), k400BadRequest)); return;
        }
        const auto exists = database->query("SELECT user_id FROM employees WHERE user_id=?", {employeeId});
        if (exists.empty()) { callback(json(error("Employee not found."), k404NotFound)); return; }
        const auto payrollRows = database->query("SELECT (SELECT b.amount FROM salary_bases b WHERE b.user_id=e.user_id AND b.effective_month<=? ORDER BY b.effective_month DESC,b.id DESC LIMIT 1) AS base_salary,COALESCE(SUM(CASE WHEN s.kind='increase' THEN s.amount ELSE -s.amount END),0) AS net FROM employees e LEFT JOIN salary_changes s ON s.user_id=e.user_id AND s.pay_month=? WHERE e.user_id=? GROUP BY e.user_id", {month, month, employeeId});
        if (payrollRows.empty() || payrollRows.front()["base_salary"].isNull()) {
            callback(json(error("Set a baseline salary effective for this month before recording an adjustment."), k409Conflict)); return;
        }
        if (kind == "decrease" && amount > payrollRows.front()["base_salary"].asInt64() + payrollRows.front()["net"].asInt64()) {
            callback(json(error("This decrease would make the calculated pay negative."), k400BadRequest)); return;
        }
        if (kind == "increase" && payrollRows.front()["net"].asInt64() + amount > payrollRows.front()["base_salary"].asInt64()) {
            callback(json(error("This increase would exceed the employee's fixed salary ceiling."), k400BadRequest)); return;
        }
        const auto id = database->insert("INSERT INTO salary_changes(user_id,pay_month,kind,amount,reason,created_by) VALUES(?,?,?,?,?,?)",
                                         {employeeId, month, kind, amount, reason, user->id});
        audit(user->id, "payroll_" + kind, employeeId, reason + " | amount=" + std::to_string(amount) + " | month=" + month);
        JsonValue body; body["id"] = static_cast<Json::Int64>(id); body["status"] = "saved";
        callback(json(std::move(body), k201Created));
    }, {Post});

    app.registerHandler("/api/payroll/base-salary", [](const HttpRequestPtr& request, std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto user = currentUser(request);
        if (!user) { callback(json(error("Sign in required."), k401Unauthorized)); return; }
        if (user->mustChangePassword) { callback(json(error("Change your temporary password first."), k403Forbidden)); return; }
        if (user->role != "payroll_admin") { callback(json(error("Payroll administrator permission required."), k403Forbidden)); return; }
        const auto input = request->getJsonObject();
        if (!input) { callback(json(error("Employee, baseline salary, and reason are required."), k400BadRequest)); return; }
        std::lock_guard payrollLock(payrollMutex);
        const auto employeeId = (*input).get("employeeId", 0).asInt64();
        const auto amount = (*input).get("amount", 0).asInt64();
        const auto reason = (*input).get("reason", "").asString();
        const auto month = (*input).get("month", "").asString();
        const bool validMonth = month.size() == 7 && month[4] == '-' && month.substr(0, 4).find_first_not_of("0123456789") == std::string::npos && month.substr(5, 2).find_first_not_of("0123456789") == std::string::npos && std::stoi(month.substr(5, 2)) >= 1 && std::stoi(month.substr(5, 2)) <= 12;
        if (employeeId <= 0 || amount < 0 || amount > 100000000 || reason.find_first_not_of(" \t\r\n") == std::string::npos || reason.size() > 240 || !validMonth) {
            callback(json(error("Baseline salary details are invalid."), k400BadRequest)); return;
        }
        const auto found = database->query("SELECT user_id,base_monthly_salary FROM employees WHERE user_id=?", {employeeId});
        if (found.empty()) { callback(json(error("Employee not found."), k404NotFound)); return; }
        const auto monthChanges = database->query("SELECT COALESCE(SUM(CASE WHEN kind='increase' THEN amount ELSE -amount END),0) AS net FROM salary_changes WHERE user_id=? AND pay_month=?", {employeeId, month});
        if (!monthChanges.empty() && amount + monthChanges.front()["net"].asInt64() < 0) { callback(json(error("This baseline would make the calculated pay negative."), k400BadRequest)); return; }
        const auto previous = database->query("SELECT amount FROM salary_bases WHERE user_id=? AND effective_month<=? ORDER BY effective_month DESC,id DESC LIMIT 1", {employeeId, month});
        database->insert("INSERT INTO salary_bases(user_id,effective_month,amount,reason,created_by) VALUES(?,?,?,?,?)", {employeeId, month, amount, reason, user->id});
        const auto currentMonth = database->query("SELECT strftime('%Y-%m','now','localtime') AS month");
        if (!currentMonth.empty() && month == currentMonth.front()["month"].asString()) database->exec("UPDATE employees SET base_monthly_salary=? WHERE user_id=?", {amount, employeeId});
        const auto oldAmount = previous.empty() ? found.front()["base_monthly_salary"].asInt64() : previous.front()["amount"].asInt64();
        audit(user->id, "baseline_salary_changed", employeeId, "month=" + month + " | old_base=" + std::to_string(oldAmount) + " | new_base=" + std::to_string(amount) + " | " + reason);
        JsonValue body; body["status"] = "updated"; body["baseSalary"] = static_cast<Json::Int64>(amount);
        callback(json(std::move(body)));
    }, {Post});
}

} // namespace

int main() {
    if (sodium_init() < 0) { std::cerr << "Could not initialize libsodium.\n"; return 1; }
    try {
        Database db;
        database = &db;
        registerRoutes();
        const char* portEnv = std::getenv("PORT");
        const int port = portEnv ? std::clamp(std::atoi(portEnv), 1, 65535) : 8080;
        std::cout << "Smart Workforce C++ API listening on http://127.0.0.1:" << port << '\n';
        const char* hostEnv = std::getenv("WORKFORCE_HOST");
        const std::string host = hostEnv && *hostEnv ? hostEnv : "127.0.0.1";
        const char* webRootEnv = std::getenv("WORKFORCE_WEB_ROOT");
        drogon::app().setDocumentRoot(webRootEnv && *webRootEnv ? webRootEnv : "../dist").setHomePage("index.html");
        drogon::app().addListener(host, static_cast<uint16_t>(port)).run();
    } catch (const std::exception& exception) {
        std::cerr << "Server startup failed: " << exception.what() << '\n';
        return 1;
    }
}
