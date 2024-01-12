#include "flight_sql_library.h"

#include <cstdlib>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <filesystem>
#include <vector>
#include <arrow/flight/client.h>
#include <arrow/flight/sql/client.h>
#include <arrow/table.h>
#include <arrow/util/logging.h>
#include <arrow/record_batch.h>
#include <boost/algorithm/string.hpp>
#include <unistd.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>

#include "sqlite/sqlite_server.h"
#include "duckdb/duckdb_server.h"
#include "flight_sql_security.h"


namespace flight = arrow::flight;
namespace flightsql = arrow::flight::sql;

const int port = 31337;

#define RUN_INIT_COMMANDS(serverType, init_sql_commands) \
    do { \
        if (init_sql_commands != "") { \
            std::vector<std::string> tokens; \
            boost::split(tokens, init_sql_commands, boost::is_any_of(";")); \
            for (const std::string &init_sql_command: tokens) { \
                if (init_sql_command.empty()) continue; \
                std::cout << "Running Init SQL command: " << std::endl << init_sql_command << ";" << std::endl; \
                ARROW_RETURN_NOT_OK(serverType->ExecuteSql(init_sql_command)); \
            } \
        } \
    } while (false)


arrow::Result<std::shared_ptr<arrow::flight::sql::FlightSqlServerBase>> ServerFactory(
        const BackendType backend,
        const fs::path &database_filename,
        const std::string &hostname,
        const std::string &username,
        const std::string &password,
        const std::string &secret_key,
        const fs::path &tls_cert_path,
        const fs::path &tls_key_path,
        const fs::path &mtls_ca_cert_path,
        const std::string &init_sql_commands,
        const bool &print_queries
) {
    ARROW_ASSIGN_OR_RAISE(auto location,
                          (!tls_cert_path.empty())
                          ? arrow::flight::Location::ForGrpcTls(
                                  hostname, port)
                          : arrow::flight::Location::ForGrpcTcp(
                                  hostname, port));

    std::cout << "Apache Arrow version: " << ARROW_VERSION_STRING << std::endl;

    arrow::flight::FlightServerOptions options(location);

    if (!tls_cert_path.empty() && !tls_key_path.empty()) {
        ARROW_CHECK_OK(arrow::flight::SecurityUtilities::FlightServerTlsCertificates(tls_cert_path, tls_key_path,
                                                                                     &options.tls_certificates));
    } else {
        std::cout << "WARNING - TLS is disabled for the Flight SQL server - this is insecure." << std::endl;
    }

    // Setup authentication middleware (using the same TLS certificate keypair)
    auto header_middleware = std::make_shared<arrow::flight::HeaderAuthServerMiddlewareFactory>(
            username, password, secret_key);
    auto bearer_middleware = std::make_shared<arrow::flight::BearerAuthServerMiddlewareFactory>(
            secret_key);

    options.auth_handler = std::make_unique<arrow::flight::NoOpAuthHandler>();
    options.middleware.push_back({"header-auth-server", header_middleware});
    options.middleware.push_back({"bearer-auth-server", bearer_middleware});

    if (!mtls_ca_cert_path.empty()) {
        std::cout << "Using mTLS CA certificate: " << mtls_ca_cert_path << std::endl;
        ARROW_CHECK_OK(arrow::flight::SecurityUtilities::FlightServerMtlsCACertificate(mtls_ca_cert_path,
                                                                                       &options.root_certificates));
        options.verify_client = true;
    }

    std::shared_ptr<arrow::flight::sql::FlightSqlServerBase> server = nullptr;

    std::string db_type = "";
    if (backend == BackendType::sqlite) {
        db_type = "SQLite";
        std::shared_ptr<arrow::flight::sql::sqlite::SQLiteFlightSqlServer> sqlite_server = nullptr;
        ARROW_ASSIGN_OR_RAISE(sqlite_server,
                              arrow::flight::sql::sqlite::SQLiteFlightSqlServer::Create(database_filename)
        )
        RUN_INIT_COMMANDS(sqlite_server, init_sql_commands);
        server = sqlite_server;
    } else if (backend == BackendType::duckdb) {
        db_type = "DuckDB";
        std::shared_ptr<arrow::flight::sql::duckdbflight::DuckDBFlightSqlServer> duckdb_server = nullptr;
        duckdb::DBConfig config;
        ARROW_ASSIGN_OR_RAISE(duckdb_server,
                              arrow::flight::sql::duckdbflight::DuckDBFlightSqlServer::Create(database_filename, config,
                                                                                              print_queries)
        )
        RUN_INIT_COMMANDS(duckdb_server, init_sql_commands);
        server = duckdb_server;
    }

    std::cout << "Using database file: " << database_filename << std::endl;

    std::cout << "Print Queries option is set to: " << std::boolalpha << print_queries << std::endl;

    if (server != nullptr) {
        ARROW_CHECK_OK(server->Init(options));

        // Exit with a clean error code (0) on SIGTERM
        ARROW_CHECK_OK(server->SetShutdownOnSignals({SIGTERM}));

        std::cout << "Apache Arrow Flight SQL server - with engine: " << db_type << " - will listen on "
                  << server->location().ToString() << std::endl;

        return server;
    } else {
        std::string err_msg = "Unable to create the Flight SQL Server";
        return arrow::Status::Invalid(err_msg);
    }
}

std::string SafeGetEnvVarValue(const std::string &env_var_name) {
    auto env_var_value = std::getenv(env_var_name.c_str());
    if (env_var_value) {
        return std::string(env_var_value);
    } else {
        return "";
    }
}

arrow::Result<std::shared_ptr<arrow::flight::sql::FlightSqlServerBase>> CreateFlightSQLServer(const BackendType backend,
                                                                                              fs::path &database_filename,
                                                                                              std::string hostname,
                                                                                              std::string username,
                                                                                              std::string password,
                                                                                              std::string secret_key,
                                                                                              fs::path tls_cert_path,
                                                                                              fs::path tls_key_path,
                                                                                              fs::path mtls_ca_cert_path,
                                                                                              std::string init_sql_commands,
                                                                                              fs::path init_sql_commands_file,
                                                                                              const bool &print_queries
) {
    // Validate and default the arguments to env var values where applicable
    if (database_filename.empty()) {
        return arrow::Status::Invalid("The database filename was not provided!");
    } else {
        // We do not check for existence of the database file, b/c they may want to create a new one
        database_filename = fs::absolute(database_filename);
    }

    if (hostname.empty()) {
        hostname = SafeGetEnvVarValue("FLIGHT_HOSTNAME");
        if (hostname.empty()) {
            hostname = "0.0.0.0";
        }
    }

    if (username.empty()) {
        return arrow::Status::Invalid("The Flight SQL Server username is empty.  You must pass a value to this argument to secure the server.");
    }

    if (password.empty()) {
        password = SafeGetEnvVarValue("FLIGHT_PASSWORD");
        if (password.empty()) {
            return arrow::Status::Invalid("The Flight SQL Server password is empty and env var: 'FLIGHT_PASSWORD' is not set.  Pass a value to this argument to secure the server.");
        }
    }

    if (secret_key.empty()) {
        secret_key = SafeGetEnvVarValue("SECRET_KEY");
        if (secret_key.empty()) {
            // Generate a random secret key
            boost::uuids::uuid uuid = boost::uuids::random_generator()();
            secret_key = "SECRET-" + boost::uuids::to_string(uuid);
        }
    }

    if (!tls_cert_path.empty()) {
        tls_cert_path = fs::absolute(tls_cert_path);
        if (!fs::exists(tls_cert_path)) {
            return arrow::Status::Invalid("TLS certificate file does not exist: " + tls_cert_path.string());
        }

        if (tls_key_path.empty()) {
            return arrow::Status::Invalid("tls_key_path was not specified (when tls_cert_path WAS specified)");
        } else {
            tls_key_path = fs::absolute(tls_key_path);
            if (!fs::exists(tls_key_path)) {
                return arrow::Status::Invalid("TLS key file does not exist: " + tls_key_path.string());
            }
        }
    }

    if (init_sql_commands.empty()) {
        init_sql_commands = SafeGetEnvVarValue("INIT_SQL_COMMANDS");
    }

    if (init_sql_commands_file.empty()) {
        init_sql_commands_file = fs::path(SafeGetEnvVarValue("INIT_SQL_COMMANDS_FILE"));
        if (!init_sql_commands_file.empty()) {
            init_sql_commands_file = fs::absolute(init_sql_commands_file);
            if (!fs::exists(init_sql_commands_file)) {
                return arrow::Status::Invalid("INIT_SQL_COMMANDS_FILE does not exist: " + init_sql_commands_file.string());
            } else {
                std::ifstream ifs(init_sql_commands_file);
                std::string init_sql_commands_file_contents((std::istreambuf_iterator<char>(ifs)),
                                                            (std::istreambuf_iterator<char>()));
                init_sql_commands += init_sql_commands_file_contents;
            }
        }
    }

    // Run additional commands for the DuckDB back-end...
    if (backend == BackendType::duckdb) {
        init_sql_commands += "SET autoinstall_known_extensions = true; SET autoload_known_extensions = true;";
    }

    if (!mtls_ca_cert_path.empty()) {
        mtls_ca_cert_path = fs::absolute(mtls_ca_cert_path);
        if (!fs::exists(mtls_ca_cert_path)) {
            return arrow::Status::Invalid("mTLS CA certificate file does not exist: " + mtls_ca_cert_path.string());
        }
    }

    return ServerFactory(backend, database_filename, hostname, username, password, secret_key,
                         tls_cert_path, tls_key_path, mtls_ca_cert_path, init_sql_commands,
                         print_queries);
}

arrow::Status StartFlightSQLServer(std::shared_ptr<arrow::flight::sql::FlightSqlServerBase> server) {

    return arrow::Status::OK();
}

int RunFlightSQLServer(const BackendType backend,
                                 fs::path &database_filename,
                                 std::string hostname,
                                 std::string username,
                                 std::string password,
                                 std::string secret_key,
                                 fs::path tls_cert_path,
                                 fs::path tls_key_path,
                                 fs::path mtls_ca_cert_path,
                                 std::string init_sql_commands,
                                 fs::path init_sql_commands_file,
                                 const bool &print_queries
) {
    auto create_server_result = CreateFlightSQLServer(backend, database_filename, hostname, username, password, secret_key,
                                                tls_cert_path, tls_key_path, mtls_ca_cert_path, init_sql_commands,
                                                init_sql_commands_file, print_queries);

    if (create_server_result.ok()) {
        auto server_ptr = create_server_result.ValueOrDie();
        std::cout << "Flight SQL server - started" << std::endl;
        ARROW_CHECK_OK(server_ptr->Serve());
        return EXIT_SUCCESS;
    } else {
        // Handle the error
        std::cerr << "Error: " << create_server_result.status().ToString() << std::endl;
        return EXIT_FAILURE;
    }
}
