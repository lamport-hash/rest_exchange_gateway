#include <crow_all.h>

#include <cstdint>
#include <string>

auto main(int a_argc, char* a_argv[]) -> int
{
    const std::uint16_t port =
        a_argc > 1 ? static_cast<std::uint16_t>(std::stoi(a_argv[1])) : 18080;

    crow::SimpleApp app;

    CROW_ROUTE(app, "/health")
    ([] {
        crow::json::wvalue body;
        body["status"] = "ok";
        return crow::response(body);
    });

    app.port(port).concurrency(2).loglevel(crow::LogLevel::Warning).run();
    return 0;
}
