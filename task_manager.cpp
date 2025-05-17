#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <string>
#include <atomic>
#include <sqlite3.h>
#include <algorithm>
#include "httplib.h"
#include <shared_mutex>
#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace httplib;
namespace fs = std::filesystem;

class Task {
private:
    int id_;
    std::string name_;
    std::string image_filename_;
    bool completed_ = false;

public:
    Task(int id, const std::string& name) : id_{id}, name_{name} {}

    void execute() {
        completed_ = true;
        std::cout << "Task '" << name_ << "' completed." << std::endl;
    }

    void set_image_filename(const std::string& filename) { image_filename_ = filename; }
    
    bool is_completed() const { return completed_; }
    int get_id() const { return id_; }
    std::string get_name() const { return name_; }
    std::string get_image_filename() const { return image_filename_; }
};

class Database {
private:
    sqlite3* db_;

public:
    Database(const std::string& database_name) {
        sqlite3_open(database_name.c_str(), &db_);
        create_table();
        fs::create_directories("./static/uploads");
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Database(Database&& other) noexcept : db_{other.db_} {
        other.db_ = nullptr;
    }

    Database& operator=(Database&& other) noexcept {
        if (this != &other) {
            sqlite3_close(db_);
            db_ = other.db_;
            other.db_ = nullptr;
        }
        return *this;
    }

    ~Database() {
        sqlite3_close(db_);
    }

    void create_table() {
        const char* sql = 
            "CREATE TABLE IF NOT EXISTS tasks ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "name TEXT NOT NULL, "
            "completed INTEGER DEFAULT 0, "
            "image_filename TEXT);";
        sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
    }

    void add_task(const std::string& name, const std::string& image_filename = "") {
        std::string sql = 
            "INSERT INTO tasks (name, image_filename) VALUES ('" + 
            name + "', '" + image_filename + "');";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

    void mark_task_completed(int id) {
        std::string sql = 
            "UPDATE tasks SET completed = 1 WHERE id = " + std::to_string(id) + ";";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

    std::vector<Task> get_tasks() {
        std::vector<Task> tasks;
        sqlite3_stmt* stmt;
        const char* sql = "SELECT * FROM tasks;";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int id = sqlite3_column_int(stmt, 0);
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                bool completed = sqlite3_column_int(stmt, 2);
                const char* image = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

                Task task(id, name);
                if (completed) task.execute();
                task.set_image_filename(image ? image : "");
                tasks.push_back(task);
            }
            sqlite3_finalize(stmt);
        }
        return tasks;
    }
};

class TaskManager {
private:
    Database& db_;
    mutable std::shared_mutex mtx_;

public:
    TaskManager(Database& db) : db_(db) {}

    void add_task(const std::string& name, const std::string& image_filename = "") {
        std::unique_lock lock(mtx_);
        db_.add_task(name, image_filename);
    }

    void complete_task(int id) {
        std::unique_lock lock(mtx_);
        db_.mark_task_completed(id);
    }

    std::vector<Task> get_tasks() const {
        std::shared_lock lock(mtx_);
        return db_.get_tasks();
    }
};

class WebInterface {
private:
    TaskManager& tm_;
    Server svr_;

public:
    WebInterface(TaskManager& tm) : tm_(tm) {
        setup_routes();
    }

    void run(int port = 8080) {
        std::cout << "Server running on http://localhost:" << port << std::endl;
        svr_.listen("0.0.0.0", port);
    }

private:
    void setup_routes() {
        svr_.Get("/tasks", [&](const Request&, Response& res) {
            auto tasks = tm_.get_tasks();
            json j;
            for (const auto& t : tasks) {
                j.push_back({
                    {"id", t.get_id()},
                    {"name", t.get_name()},
                    {"completed", t.is_completed()},
                    {"image_filename", t.get_image_filename()}
                });
            }
            res.set_content(j.dump(), "application/json");
        });

        svr_.Post("/tasks", [&](const Request& req, Response& res) {
            auto j = json::parse(req.body);
            tm_.add_task(
                j["name"].get<std::string>(),
                j.value("image_filename", "")
            );
            res.status = 201;
        });

        svr_.Put(R"(/tasks/(\d+))", [&](const Request& req, Response& res) {
            int id = std::stoi(req.matches[1]);
            tm_.complete_task(id);
            res.status = 200;
        });

        svr_.Post("/upload", [&](const Request& req, Response& res) {
            const auto& file = req.get_file_value("image");
            std::string path = "./static/uploads/" + file.filename;
            
            std::ofstream ofs(path, std::ios::binary);
            ofs << file.content;
            ofs.close();

            json response;
            response["filename"] = file.filename;
            res.set_content(response.dump(), "application/json");
        });

        svr_.set_mount_point("/", "./static");
        svr_.set_mount_point("/uploads", "./static/uploads");
    }
};

int main() {
    Database db("tasks.db");
    TaskManager tm(db);
    
    WebInterface web(tm);
    std::thread web_thread([&web] { web.run(); });
    
    web_thread.join();
    return 0;
}
