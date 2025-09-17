#include <iostream>
#include <vector>
#include <algorithm>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rtdb.h"

#include "value.h"
#include "json.h"
#define RTDB_TAG "RTDB"


namespace ESPFirebase {


RTDB::RTDB(FirebaseApp* app, const char * database_url)
    : app(app), base_database_url(database_url)

{
    
}
Json::Value RTDB::getData(const char* path)
{
    
    std::string url = RTDB::base_database_url;
    url += path;
    url += ".json?auth=" + this->app->auth_token;

    this->app->setHeader("content-type", "application/json");
    http_ret_t http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_GET, "");
    if (http_ret.err == ESP_OK && http_ret.status_code == 200)
    {
        const char* begin = this->app->local_response_buffer;
        const char* end = begin + strlen(this->app->local_response_buffer);

        Json::Reader reader;
        Json::Value data;

        reader.parse(begin, end, data, false);

        ESP_LOGI(RTDB_TAG, "Data with path=%s acquired", path);
        this->app->clearHTTPBuffer();
        return data;
    }
    else
    {   
        ESP_LOGE(RTDB_TAG, "Error while getting data at path %s| esp_err_t=%d | status_code=%d", path, (int)http_ret.err, http_ret.status_code);
    ESP_LOGI(RTDB_TAG, "Token expired ? Trying refreshing auth");
    this->app->loginUserAccount(this->app->user_account);
        this->app->setHeader("content-type", "application/json");
        http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_GET, "");
        if (http_ret.err == ESP_OK && http_ret.status_code == 200)
        {
            const char* begin = this->app->local_response_buffer;
            const char* end = begin + strlen(this->app->local_response_buffer);

            Json::Reader reader;
            Json::Value data;

            reader.parse(begin, end, data, false);

            ESP_LOGI(RTDB_TAG, "Data with path=%s acquired", path);
            this->app->clearHTTPBuffer();
            return data;
        }
        else
        {
            ESP_LOGE(RTDB_TAG, "Failed to get data after refreshing token. double check account credentials or database rules");
            this->app->clearHTTPBuffer();
            return Json::Value();
        }
    }
}

esp_err_t RTDB::putData(const char* path, const char* json_str)
{
    
    std::string url = RTDB::base_database_url;
    url += path;
    url += ".json?auth=" + this->app->auth_token;
    this->app->setHeader("content-type", "application/json");
    http_ret_t http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_PUT, json_str);
    if (!(http_ret.err == ESP_OK && http_ret.status_code == 200) && http_ret.status_code == 401) {
        ESP_LOGW(RTDB_TAG, "PUT 401 -> intentando refresh auth");
        this->app->forceRefreshAuth();
        url = RTDB::base_database_url; url += path; url += ".json?auth=" + this->app->auth_token;
        this->app->setHeader("content-type", "application/json");
        http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_PUT, json_str);
    }
    this->app->clearHTTPBuffer();
    if (http_ret.err == ESP_OK && http_ret.status_code == 200) {
        ESP_LOGI(RTDB_TAG, "PUT successful");
        return ESP_OK;
    }
    ESP_LOGE(RTDB_TAG, "PUT failed");
    return ESP_FAIL;
}

esp_err_t RTDB::putData(const char* path, const Json::Value& data)
{
    Json::FastWriter writer;
    std::string json_str = writer.write(data);
    esp_err_t err = RTDB::putData(path, json_str.c_str());
    return err;

}

esp_err_t RTDB::postData(const char* path, const char* json_str)
{
    
    std::string url = RTDB::base_database_url;
    url += path;
    url += ".json?auth=" + this->app->auth_token;
    this->app->setHeader("content-type", "application/json");
    http_ret_t http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_POST, json_str);
    if (!(http_ret.err == ESP_OK && http_ret.status_code == 200) && http_ret.status_code == 401) {
        ESP_LOGW(RTDB_TAG, "POST 401 -> intentando refresh auth");
        this->app->forceRefreshAuth();
        url = RTDB::base_database_url; url += path; url += ".json?auth=" + this->app->auth_token;
        this->app->setHeader("content-type", "application/json");
        http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_POST, json_str);
    }
    this->app->clearHTTPBuffer();
    if (http_ret.err == ESP_OK && http_ret.status_code == 200) {
        ESP_LOGI(RTDB_TAG, "POST successful");
        return ESP_OK;
    }
    ESP_LOGE(RTDB_TAG, "POST failed");
    return ESP_FAIL;
}

esp_err_t RTDB::postData(const char* path, const Json::Value& data)
{
    Json::FastWriter writer;
    std::string json_str = writer.write(data);
    esp_err_t err = RTDB::postData(path, json_str.c_str());
    return err;

}
esp_err_t RTDB::patchData(const char* path, const char* json_str)
{
    
    std::string url = RTDB::base_database_url;
    url += path;
    url += ".json?auth=" + this->app->auth_token;
    this->app->setHeader("content-type", "application/json");
    http_ret_t http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_PATCH, json_str);
    if (!(http_ret.err == ESP_OK && http_ret.status_code == 200) && http_ret.status_code == 401) {
        ESP_LOGW(RTDB_TAG, "PATCH 401 -> intentando refresh auth");
        this->app->forceRefreshAuth();
        url = RTDB::base_database_url; url += path; url += ".json?auth=" + this->app->auth_token;
        this->app->setHeader("content-type", "application/json");
        http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_PATCH, json_str);
    }
    this->app->clearHTTPBuffer();
    if (http_ret.err == ESP_OK && http_ret.status_code == 200) {
        ESP_LOGI(RTDB_TAG, "PATCH successful");
        return ESP_OK;
    }
    ESP_LOGE(RTDB_TAG, "PATCH failed");
    return ESP_FAIL;
}

esp_err_t RTDB::patchData(const char* path, const Json::Value& data)
{
    Json::FastWriter writer;
    std::string json_str = writer.write(data);
    esp_err_t err = RTDB::patchData(path, json_str.c_str());
    return err;
}


esp_err_t RTDB::deleteData(const char* path)
{
    std::string url = RTDB::base_database_url;
    url += path;
    // Agregamos print=silent para que el servidor no devuelva el payload eliminado
    url += ".json?auth=" + this->app->auth_token + "&print=silent";

    // DELETE sin cuerpo: Content-Length: 0 y aceptar JSON (aunque con print=silent podría devolver 204)
    this->app->setHeader("Content-Length", "0");
    this->app->setHeader("Accept", "application/json");

    http_ret_t http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_DELETE, "");

    // Si el token expiró
    if (!(http_ret.err == ESP_OK && (http_ret.status_code >= 200 && http_ret.status_code < 300)) &&
        http_ret.status_code == 401) {
        ESP_LOGW(RTDB_TAG, "DELETE 401 -> intentando refresh auth");
        this->app->forceRefreshAuth();
        url = RTDB::base_database_url; url += path; url += ".json?auth=" + this->app->auth_token + "&print=silent";
        this->app->setHeader("Content-Length", "0");
        this->app->setHeader("Accept", "application/json");
        http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_DELETE, "");
    }

    if (http_ret.err == ESP_OK && (http_ret.status_code >= 200 && http_ret.status_code < 300)) {
        this->app->clearHTTPBuffer();
        ESP_LOGI(RTDB_TAG, "DELETE successful (status=%d)", http_ret.status_code);
        return ESP_OK;
    }

    // Fallback: si el nodo es muy grande, borrar por lotes usando lista shallow de hijos
    bool maybe_too_large = (http_ret.status_code == 400);
    if (!maybe_too_large) {
        ESP_LOGE(RTDB_TAG, "DELETE failed (status=%d)", http_ret.status_code);
        this->app->clearHTTPBuffer();
        return ESP_FAIL;
    }

    ESP_LOGW(RTDB_TAG, "DELETE grande: intentando borrado por lotes (shallow)");

    // Paginación sin shallow: orderBy=$key y limitToFirst para no desbordar el buffer
    const int BATCH = 50; // ajustable: payload reducido
    std::string last_key;
    bool have_last = false;
    size_t total_ok = 0;
    size_t total_seen = 0;

    while (true) {
        std::string list_url = RTDB::base_database_url;
        list_url += path;
        list_url += ".json?orderBy=%22%24key%22";
        int limit = BATCH + (have_last ? 1 : 0);
        list_url += "&limitToFirst=" + std::to_string(limit);
        if (have_last) {
            list_url += "&startAt=%22" + last_key + "%22";
        }
        list_url += "&auth=" + this->app->auth_token;

        this->app->setHeader("content-type", "application/json");
        http_ret_t get_ret = this->app->performRequest(list_url.c_str(), HTTP_METHOD_GET, "");
        if (!(get_ret.err == ESP_OK && get_ret.status_code == 200)) {
            ESP_LOGE(RTDB_TAG, "Fallo obteniendo claves (paged) status=%d", get_ret.status_code);
            this->app->clearHTTPBuffer();
            return ESP_FAIL;
        }

        const char* begin = this->app->local_response_buffer;
        const char* end = begin + strlen(this->app->local_response_buffer);
        Json::Reader reader;
        Json::Value keys_obj;
        reader.parse(begin, end, keys_obj, false);
        this->app->clearHTTPBuffer();

        if (!keys_obj.isObject() || keys_obj.getMemberNames().empty()) {
            break; // no hay más
        }

        std::vector<std::string> keys = keys_obj.getMemberNames();

        // Si usamos startAt, el primer elemento puede ser igual a last_key; hay que saltarlo
        size_t start_index = 0;
        if (have_last) {
            // buscar y saltar la coincidencia
            for (size_t i = 0; i < keys.size(); ++i) {
                if (keys[i] == last_key) { start_index = i + 1; break; }
            }
        }

        // Determinar nuevo last_key: máximo lexicográfico del lote
        std::string new_last = keys[start_index > 0 ? start_index - 1 : 0];
        for (size_t i = start_index; i < keys.size(); ++i) {
            if (keys[i] > new_last) new_last = keys[i];
        }

        // Intentar borrar el lote con un PATCH {"k1":null, ...}
        std::string patch_body;
        patch_body.reserve(2048);
        patch_body += "{";
        size_t added = 0;
        for (size_t i = start_index; i < keys.size(); ++i) {
            const std::string& k = keys[i];
            if (added > 0) patch_body += ",";
            patch_body += "\"";
            patch_body += k;
            patch_body += "\":null";
            added++;
        }
        patch_body += "}";

        // PATCH en el padre con print=silent
        std::string patch_url = RTDB::base_database_url;
        patch_url += path;
        patch_url += ".json?auth=" + this->app->auth_token + "&print=silent";
        this->app->setHeader("content-type", "application/json");
        http_ret_t patch_ret = this->app->performRequest(patch_url.c_str(), HTTP_METHOD_PATCH, patch_body);
        if (!(patch_ret.err == ESP_OK && patch_ret.status_code >= 200 && patch_ret.status_code < 300)) {
            // Si falla el PATCH del lote, hacer fallback a borrar hijo por hijo
            ESP_LOGW(RTDB_TAG, "PATCH de lote fallo (status=%d). Fallback por hijos", patch_ret.status_code);
            for (size_t i = start_index; i < keys.size(); ++i) {
                const std::string& k = keys[i];
                std::string child_path = std::string(path) + "/" + k;
                if (RTDB::deleteData(child_path.c_str()) == ESP_OK) {
                    total_ok++;
                } else {
                    ESP_LOGW(RTDB_TAG, "Fallo al borrar hijo: %s", child_path.c_str());
                }
                total_seen++;
                if ((i - start_index + 1) % 20 == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            total_ok += added;
            total_seen += added;
        }

        // Si recibimos menos de 'limit' elementos, no hay más páginas
        if (keys.size() < (size_t)limit) {
            break;
        }
        last_key = new_last;
        have_last = true;
        // breve respiro
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Intentar borrar el padre al final
    std::string final_url = RTDB::base_database_url;
    final_url += path;
    final_url += ".json?auth=" + this->app->auth_token + "&print=silent";
    this->app->setHeader("Content-Length", "0");
    this->app->setHeader("Accept", "application/json");
    http_ret_t final_ret = this->app->performRequest(final_url.c_str(), HTTP_METHOD_DELETE, "");
    this->app->clearHTTPBuffer();
    if (final_ret.err == ESP_OK && (final_ret.status_code >= 200 && final_ret.status_code < 300)) {
        ESP_LOGI(RTDB_TAG, "DELETE por lotes paginado OK (borrados %u hijos)", (unsigned)total_ok);
        return ESP_OK;
    }

    ESP_LOGE(RTDB_TAG, "DELETE final fallo (status=%d) tras borrado paginado (ok=%u, vistos=%u)", final_ret.status_code, (unsigned)total_ok, (unsigned)total_seen);
    return total_ok > 0 ? ESP_OK : ESP_FAIL;
}



esp_err_t RTDB::trimDays(const char* root_path, int max_days)
{
    if (max_days <= 0) return ESP_OK;

    // Listar días (claves) bajo root con shallow=true
    std::string url = RTDB::base_database_url;
    url += root_path;
    url += ".json?shallow=true&auth=" + this->app->auth_token;
    this->app->setHeader("content-type", "application/json");
    http_ret_t http_ret = this->app->performRequest(url.c_str(), HTTP_METHOD_GET, "");
    if (!(http_ret.err == ESP_OK && http_ret.status_code == 200)) {
        ESP_LOGE(RTDB_TAG, "trimDays: fallo GET shallow status=%d", http_ret.status_code);
        this->app->clearHTTPBuffer();
        return ESP_FAIL;
    }

    const char* begin = this->app->local_response_buffer;
    const char* end = begin + strlen(this->app->local_response_buffer);
    Json::Reader reader;
    Json::Value days_obj;
    reader.parse(begin, end, days_obj, false);
    this->app->clearHTTPBuffer();
    if (!days_obj.isObject()) return ESP_OK; // nada que recortar

    std::vector<std::string> days = days_obj.getMemberNames();
    if ((int)days.size() <= max_days) return ESP_OK;

    // Las fechas deben estar en formato YYYY-MM-DD para que el orden lex sea cronológico
    std::sort(days.begin(), days.end()); // asc: más antiguo primero si YYYY-MM-DD

    int to_delete = (int)days.size() - max_days;
    for (int i = 0; i < to_delete; ++i) {
        std::string child = std::string(root_path) + "/" + days[i];
        ESP_LOGI(RTDB_TAG, "trimDays: borrando día antiguo %s", days[i].c_str());
        RTDB::deleteData(child.c_str());
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

// Borra los N elementos más antiguos bajo root_path usando orderBy=$key y PATCH con print=silent
int RTDB::trimOldestBatch(const char* root_path, int batch_size)
{
    if (batch_size <= 0) return 0;
    std::string list_url = RTDB::base_database_url;
    list_url += root_path;
    list_url += ".json?orderBy=%22%24key%22&limitToFirst=" + std::to_string(batch_size) + "&auth=" + this->app->auth_token;
    this->app->setHeader("content-type", "application/json");
    http_ret_t get_ret = this->app->performRequest(list_url.c_str(), HTTP_METHOD_GET, "");
    if (!(get_ret.err == ESP_OK && get_ret.status_code == 200)) {
        this->app->clearHTTPBuffer();
        return -1;
    }
    const char* begin = this->app->local_response_buffer;
    const char* end = begin + strlen(this->app->local_response_buffer);
    Json::Reader reader;
    Json::Value obj;
    reader.parse(begin, end, obj, false);
    this->app->clearHTTPBuffer();
    if (!obj.isObject()) return 0;
    std::vector<std::string> keys = obj.getMemberNames();
    if (keys.empty()) return 0;

    std::string patch_body;
    patch_body.reserve(1024);
    patch_body += "{";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) patch_body += ",";
        patch_body += "\""; patch_body += keys[i]; patch_body += "\":null";
    }
    patch_body += "}";

    std::string patch_url = RTDB::base_database_url;
    patch_url += root_path;
    patch_url += ".json?auth=" + this->app->auth_token + "&print=silent";
    this->app->setHeader("content-type", "application/json");
    http_ret_t patch_ret = this->app->performRequest(patch_url.c_str(), HTTP_METHOD_PATCH, patch_body);
    this->app->clearHTTPBuffer();
    if (!(patch_ret.err == ESP_OK && patch_ret.status_code >= 200 && patch_ret.status_code < 300)) {
        return -2;
    }
    return (int)keys.size();
}

}
