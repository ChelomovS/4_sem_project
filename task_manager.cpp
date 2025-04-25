#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <string>
#include <atomic>
#include <sqlite3.h>
#include <algorithm>
#include <httplib.h>
#include <shared_mutex>

#include "nlohmann/json.hpp"

using json = nlohmann::json; // 
using namespace httplib;

class Task {
private:
    int id_;
    std::string name_;
    bool completed_ = false;

public:
    Task(int id, const std::string& name) : id_{id}, name_{name} {}

    void execute() {
        std::this_thread::sleep_for(std::chrono::seconds(1)); // to simulate work
        completed_ = true;
        std::cout << "Task '" << name_ << "' completed." << std::endl;
    }

    bool is_completed() const {
        return completed_;
    }

    int get_id() const {
        return id_;
    }

    std::string get_name() const {
        return name_;
    }
};

class Database {
    // RAII class - resource is database
private:
    sqlite3* db_;
public:
    // Rule of 5
    Database(const std::string& database_name) {
        sqlite3_open(database_name.c_str(), &db_);
        create_table();
    }

    Database(Database& other) = delete;

    Database& operator=(Database& other) = delete;

    Database(Database&& other) noexcept : db_{other.db_} {
        other.db_ = nullptr;
    }

    Database& operator=(Database&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        std::swap(db_, other.db_);
        return *this;
    }

    ~Database() {
        sqlite3_close(db_);
    }

    void create_table() {
        // CREATE TABLE IF NOT EXISTS – создает таблицу, если она не существует
        // tasks - имя таблицы
        // PRIMARY KEY - основной ключ 
        // name - название задачи 
        // completed - статус - выполнена / невыполнена, INTEGER - 0/1 - аналог bool
        std::string sql = "CREATE TABLE IF NOT EXISTS tasks (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, completed INTEGER);";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

    void add_task(const std::string& task_name) {
        // INSERT INTO - вставка новой задачи 
        // (name, completed) - список полей для инициализации 
        // VALUES - значения для инициализации - строка имени и состояние "невыполнена"
        std::string sql = "INSERT INTO tasks (name, completed) VALUES ('" + task_name + "', 0);";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

    void mark_task_completed(int task_id) {
        // UPDATE – обновление базы 
        // SET completed = 1 – выставляем значение completed 
        // WHERE id - выбор записи в базе по id 
        std::string sql = "UPDATE tasks SET completed = 1 WHERE id = " + std::to_string(task_id) + ";";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

    std::vector<Task> get_tasks() {
        std::vector<Task> tasks; // used vector 
        // хотии получить все задачи из базы и вернуть std::vector задач по значению
        const char* sql = "SELECT * FROM tasks;";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int id = sqlite3_column_int(stmt, 0);
                // id - id из 0 столбца
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                // name - название задачи из 1 столбца
                bool completed = sqlite3_column_int(stmt, 2);
                // completed - состояние из 2 столбца базы данных
                tasks.emplace_back(id, name);
                if (completed) {
                    tasks.back().execute(); 
                    mark_task_completed(id);
                }
            }
            // завершение задачи
            sqlite3_finalize(stmt);
        }
        return tasks;
    }
};

class TaskManager {
private:
    Database& database_;
    std::shared_mutex mtx_; 
public:
    TaskManager(Database& database) : database_{database} {}

    void add_task(const std::string& name) {
        std::unique_lock lock(mtx_); // уникальное владение
        database_.add_task(name);
    }

    void complete_task(int id) {
        std::unique_lock lock(mtx_);  // уникальное владение 
        database_.mark_task_completed(id);
    }

    std::vector<Task> get_tasks() {
        std::shared_lock lock(mtx_);  // разделяемое владение 
        return database_.get_tasks();
    }
};

class UserInterface {
private:
    TaskManager& manager_;

public:
    UserInterface(TaskManager& mgr) : manager_{mgr} {}

    void show_menu() {
        std::cout << "1. Add Task\n2. Complete Task\n3. Show Tasks\n4. Exit\n";
        int choice;
        std::cin >> choice;

        switch (choice) {
            case 1: {
                std::string name;
                std::cout << "Enter task name: ";
                std::cin >> name;
                manager_.add_task(name);
                break;
            }

            case 2: {
                int id;
                std::cout << "Enter task ID to complete: ";
                std::cin >> id;
                manager_.complete_task(id);
                break;
            }

            case 3: {
                auto&& tasks = manager_.get_tasks();
                auto&& print_if_not_empty = [] (const auto& task) {
                    if (task.get_name().empty()) 
                        return;
        
                    std::cout << "ID: " << task.get_id() << ", Name: " << task.get_name() 
                              << ", Completed: " << (task.is_completed() ? "Yes" : "No") << std::endl;
                };

                std::for_each(tasks.begin(), tasks.end(), print_if_not_empty);
                break;
            }

            case 4:
                return; 

            default:
                std::cout << "Invalid choice!" << std::endl;
            }
        
        show_menu();
    }
};

class WebInterface {
private:
    TaskManager& manager_;
    Server svr_;

public:
    WebInterface(TaskManager& mgr) : manager_{mgr} {
        setup_routes();
    }

    void run(int port = 8080) {
        std::cout << "Server running on http://localhost:" << port << std::endl; // ссылка на веб интерфейс 
        svr_.listen("0.0.0.0", port);
    }

    void setup_routes() {
        // get all tasks
        svr_.Get("/tasks", [&](const Request& req, Response& res) { 
            auto tasks = manager_.get_tasks();
            // json используется для REST API для обмена данными 
            json j; 
            for (auto&& task : tasks) {
                j.push_back({
                    {"id", task.get_id()},              // добавляем id 
                    {"name", task.get_name()},          // добавляем имя 
                    {"completed", task.is_completed()}  // добавляем состояние задачи 
                }); // push back добавляем элемент в json массив 
            }
            res.set_content(j.dump(), "application/json"); // j.dump() - преобразует JSON обьект в строку 
        });

        // post task
        svr_.Post("/tasks", [&](const Request& req, Response& res) {
            auto j = json::parse(req.body); // json.parse(req.body) - парсит строку в JSON обьект 
            manager_.add_task(j["name"].get<std::string>());
            res.status = 201; // успешное создание
        });

        // complete task
        svr_.Put(R"(/tasks/(\d+))", [&](const Request& req, Response& res) {
            int id = std::stoi(req.matches[1]);
            manager_.complete_task(id);
            res.status = 200;
        });

        svr_.set_mount_point("/", "./static"); 
    }
};

int main() {
    Database database("tasks.db");
    TaskManager manager(database); // гарантирует безопасность относительно многопоточного окружения 
    // гарантирует безопасность относительно операций добавления, отметки выполнения, чтения списка задач
    
    WebInterface web(manager);
    // used new thread for web 
    std::thread web_thread([&web]() {
        web.run(); // default value 
    });
    
    UserInterface ui(manager);
    // основная функция главного потока - управление консольным интерфейсом (ui.show_menu())
    // общий ресурс двух потоков - база данных - файл tasks.db
    ui.show_menu();
    
    web_thread.join();
    return 0;
}