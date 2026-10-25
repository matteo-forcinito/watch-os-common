#include "life_model.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *life_string(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
int life_number(const cJSON *o, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) && v->valuedouble >= INT_MIN && v->valuedouble <= INT_MAX &&
                   v->valuedouble == (double)v->valueint
               ? v->valueint
               : fallback;
}
static bool leap(int y)
{
    return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}
static int year_start(int y)
{
    y--;
    return y * 365 + y / 4 - y / 100 + y / 400 - 719162;
}
int life_day(const char *s)
{
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-')
        return LIFE_NO_DAY;
    for (int i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdigit((unsigned char)s[i]))
            return LIFE_NO_DAY;
    int y = atoi(s), m = atoi(s + 5), d = atoi(s + 8);
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 1900 || y > 2199 || m < 1 || m > 12 || d < 1 || d > days[m - 1] + (m == 2 && leap(y)))
        return LIFE_NO_DAY;
    int n = year_start(y) + d - 1;
    for (int i = 1; i < m; i++)
        n += days[i - 1] + (i == 2 && leap(y));
    return n;
}
void life_date(int day, char out[11])
{
    if (day < year_start(1900) || day >= year_start(2200))
    {
        out[0] = 0;
        return;
    }
    int lo = 1900, hi = 2200;
    while (lo + 1 < hi)
    {
        int mid = (lo + hi) / 2;
        if (year_start(mid) <= day)
            lo = mid;
        else
            hi = mid;
    }
    int y = lo, m = 1, d = day - year_start(y);
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    while (d >= days[m - 1] + (m == 2 && leap(y)))
    {
        d -= days[m - 1] + (m == 2 && leap(y));
        m++;
    }
    unsigned year = (unsigned)y, month = (unsigned)m, date = (unsigned)d + 1;
    out[0] = '0' + year / 1000 % 10;
    out[1] = '0' + year / 100 % 10;
    out[2] = '0' + year / 10 % 10;
    out[3] = '0' + year % 10;
    out[4] = '-';
    out[5] = '0' + month / 10;
    out[6] = '0' + month % 10;
    out[7] = '-';
    out[8] = '0' + date / 10;
    out[9] = '0' + date % 10;
    out[10] = 0;
}
int life_weekday(int day)
{
    int w = (day + 3) % 7;
    return w < 0 ? w + 7 : w;
}
const cJSON *life_find(const cJSON *a, int id)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, a) if (life_number(v, "id", -1) == id) return v;
    return NULL;
}
static cJSON *find(cJSON *a, int id)
{
    return (cJSON *)life_find(a, id);
}
static void number(cJSON *o, const char *k, int v)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    cJSON_AddNumberToObject(o, k, v);
}
static void text(cJSON *o, const char *k, const char *v)
{
    cJSON *s = cJSON_CreateString(v);
    if (!s)
        return;
    if (!cJSON_ReplaceItemInObjectCaseSensitive(o, k, s))
        cJSON_AddItemToObject(o, k, s);
}
static void boolean(cJSON *o, const char *k, bool v)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    cJSON_AddBoolToObject(o, k, v);
}
static bool fail(char *e, size_t n, const char *s)
{
    if (e && n)
        snprintf(e, n, "%s", s);
    return false;
}
static int next_id(cJSON *d)
{
    int n = life_number(d, "next_id", 1);
    if (n < 1 || n >= INT_MAX - 1)
        return -1;
    number(d, "next_id", n + 1);
    return n;
}
static void erase_id(cJSON *a, int id)
{
    for (int i = cJSON_GetArraySize(a) - 1; i >= 0; i--)
        if (life_number(cJSON_GetArrayItem(a, i), "id", -1) == id)
            cJSON_DeleteItemFromArray(a, i);
}
static void erase_pet(cJSON *a, int id)
{
    for (int i = cJSON_GetArraySize(a) - 1; i >= 0; i--)
        if (life_number(cJSON_GetArrayItem(a, i), "pet", -1) == id)
            cJSON_DeleteItemFromArray(a, i);
}
static bool field(cJSON *o, const cJSON *c, const char *k, size_t max, bool required, char *e, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(c, k);
    if (!v)
        return !required || fail(e, n, "Campo obbligatorio mancante");
    if (!cJSON_IsString(v) || strlen(v->valuestring) > max || (!v->valuestring[0] && required))
        return fail(e, n, "Testo vuoto o troppo lungo");
    text(o, k, v->valuestring);
    return true;
}
cJSON *life_defaults(life_kind_t kind)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "schema", 1);
    cJSON_AddNumberToObject(d, "next_id", 1);
    if (kind == LIFE_RACCOLTA)
    {
        cJSON_AddArrayToObject(d, "cities");
        number(d, "preferred", 0);
        boolean(d, "menu_hint", false);
    }
    else
    {
        cJSON_AddArrayToObject(d, "pets");
        cJSON_AddArrayToObject(d, "tasks");
        cJSON_AddArrayToObject(d, "weights");
        cJSON_AddArrayToObject(d, "logs");
    }
    return d;
}
static cJSON *bin_new(int id, const char *name, int color, int days)
{
    cJSON *b = cJSON_CreateObject();
    number(b, "id", id);
    text(b, "name", name);
    number(b, "color", color);
    number(b, "days", days);
    number(b, "every", 1);
    text(b, "anchor", "2025-01-13");
    return b;
}
cJSON *life_collections(const cJSON *city, int day)
{
    cJSON *out = cJSON_CreateArray();
    if (!city || day == LIFE_NO_DAY)
        return out;
    char date[11];
    life_date(day, date);
    const cJSON *override = NULL, *v;
    cJSON_ArrayForEach(v,
                       cJSON_GetObjectItemCaseSensitive(city, "exceptions")) if (!strcmp(life_string(v, "date"), date))
    {
        override = v;
        break;
    }
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(city, "bins"))
    {
        bool yes = false;
        if (override)
        {
            const cJSON *id;
            cJSON_ArrayForEach(id, cJSON_GetObjectItemCaseSensitive(
                                       override, "bins")) if (cJSON_IsNumber(id) &&
                                                              id->valueint == life_number(v, "id", 0)) yes = true;
        }
        else
        {
            int every = life_number(v, "every", 1), anchor = life_day(life_string(v, "anchor"));
            if (anchor == LIFE_NO_DAY)
                anchor = life_day("1970-01-05");
            int week = (day - anchor) / 7;
            yes = (life_number(v, "days", 0) & (1 << life_weekday(day))) != 0 && every >= 1 && every <= 8 &&
                  day >= anchor && week % every == 0;
        }
        if (yes)
            cJSON_AddItemToArray(out, cJSON_Duplicate(v, true));
    }
    return out;
}
static bool waste(cJSON *d, const cJSON *c, int today, char *e, size_t n)
{
    (void)today;
    const char *op = life_string(c, "op");
    cJSON *cities = cJSON_GetObjectItemCaseSensitive(d, "cities");
    int id = life_number(c, "city", 0);
    cJSON *city = find(cities, id);
    if (!strcmp(op, "city.add"))
    {
        if (cJSON_GetArraySize(cities) >= 8)
            return fail(e, n, "Massimo 8 citta");
        city = cJSON_CreateObject();
        number(city, "id", next_id(d));
        text(city, "zone", "");
        text(city, "source", "");
        cJSON *bins = cJSON_AddArrayToObject(city, "bins");
        cJSON_AddArrayToObject(city, "exceptions");
        if (!field(city, c, "name", 48, true, e, n))
        {
            cJSON_Delete(city);
            return false;
        }
        if (!strcmp(life_string(c, "preset"), "civita-domestica"))
        {
            text(city, "source", "https://www.satespa.it/images/domestico.pdf");
            text(city, "zone", "Utenze domestiche - SATE, dal 13/01/2025");
            cJSON_AddItemToArray(bins, bin_new(next_id(d), "Organico", 0x9F6B45, (1 << 0) | (1 << 3) | (1 << 5)));
            cJSON_AddItemToArray(bins, bin_new(next_id(d), "Indifferenziato", 0x8E98A6, 1 << 1));
            cJSON_AddItemToArray(bins, bin_new(next_id(d), "Plastica e metallo", 0xF3C746, 1 << 2));
            cJSON_AddItemToArray(bins, bin_new(next_id(d), "Vetro", 0x48B782, 1 << 3));
            cJSON_AddItemToArray(bins, bin_new(next_id(d), "Carta e cartone", 0x70B4EB, 1 << 4));
        }
        cJSON_AddItemToArray(cities, city);
        if (!life_number(d, "preferred", 0))
            number(d, "preferred", life_number(city, "id", 0));
        return true;
    }
    if (!strcmp(op, "settings"))
    {
        if (cJSON_HasObjectItem(c, "preferred"))
        {
            int p = life_number(c, "preferred", 0);
            if (p && !find(cities, p))
                return fail(e, n, "Citta non trovata");
            number(d, "preferred", p);
        }
        if (cJSON_HasObjectItem(c, "menu_hint"))
            boolean(d, "menu_hint", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "menu_hint")));
        return true;
    }
    if (!city)
        return fail(e, n, "Citta non trovata");
    if (!strcmp(op, "city.edit"))
        return field(city, c, "name", 48, false, e, n) && field(city, c, "zone", 96, false, e, n) &&
               field(city, c, "source", 180, false, e, n);
    if (!strcmp(op, "city.delete"))
    {
        erase_id(cities, id);
        if (life_number(d, "preferred", 0) == id)
            number(d, "preferred", life_number(cJSON_GetArrayItem(cities, 0), "id", 0));
        return true;
    }
    cJSON *bins = cJSON_GetObjectItemCaseSensitive(city, "bins");
    if (!strcmp(op, "bin.add") || !strcmp(op, "bin.edit"))
    {
        bool add = !strcmp(op, "bin.add");
        if (add && cJSON_GetArraySize(bins) >= 12)
            return fail(e, n, "Massimo 12 raccolte per citta");
        cJSON *b = add ? bin_new(next_id(d), "", 0x48B782, 0) : find(bins, life_number(c, "id", 0));
        if (!b)
            return fail(e, n, "Raccolta non trovata");
        if (add)
            cJSON_AddItemToArray(bins, b);
        if (!field(b, c, "name", 48, add, e, n))
            return false;
        int days = life_number(c, "days", life_number(b, "days", 0)),
            every = life_number(c, "every", life_number(b, "every", 1));
        if (days < 0 || days > 127 || every < 1 || every > 8)
            return fail(e, n, "Giorni o intervallo non validi");
        if (cJSON_HasObjectItem(c, "anchor") && life_day(life_string(c, "anchor")) == LIFE_NO_DAY)
            return fail(e, n, "Data di partenza non valida");
        number(b, "days", days);
        number(b, "every", every);
        number(b, "color", life_number(c, "color", life_number(b, "color", 0x48B782)) & 0xffffff);
        return field(b, c, "anchor", 10, false, e, n);
    }
    if (!strcmp(op, "bin.delete"))
    {
        int removed = life_number(c, "id", 0);
        erase_id(bins, removed);
        cJSON *exception;
        cJSON_ArrayForEach(exception,cJSON_GetObjectItemCaseSensitive(city,"exceptions")) {
            cJSON *ids=cJSON_GetObjectItemCaseSensitive(exception,"bins");
            for(int i=cJSON_GetArraySize(ids)-1;i>=0;i--)
                if(cJSON_GetArrayItem(ids,i)->valueint==removed)cJSON_DeleteItemFromArray(ids,i);
        }
        return true;
    }
    if (!strcmp(op, "exception.put") || !strcmp(op, "exception.delete"))
    {
        const char *date = life_string(c, "date");
        if (life_day(date) == LIFE_NO_DAY)
            return fail(e, n, "Data non valida");
        cJSON *a = cJSON_GetObjectItemCaseSensitive(city, "exceptions");
        for (int i = cJSON_GetArraySize(a) - 1; i >= 0; i--)
            if (!strcmp(life_string(cJSON_GetArrayItem(a, i), "date"), date))
                cJSON_DeleteItemFromArray(a, i);
        if (!strcmp(op, "exception.delete"))
            return true;
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(c, "bins"), *v;
        if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) > 12 || cJSON_GetArraySize(a) >= 48)
            return fail(e, n, "Eccezioni non valide o limite raggiunto");
        cJSON_ArrayForEach(v, ids) if (!cJSON_IsNumber(v) || !find(bins, v->valueint)) return fail(
            e, n, "Raccolta dell'eccezione non trovata");
        cJSON *x = cJSON_CreateObject();
        text(x, "date", date);
        cJSON_AddItemToObject(x, "bins", cJSON_Duplicate(ids, true));
        cJSON_AddItemToArray(a, x);
        return true;
    }
    return fail(e, n, "Comando non riconosciuto");
}
static bool dogs(cJSON *d, const cJSON *c, int today, char *e, size_t n)
{
    const char *op = life_string(c, "op");
    cJSON *pets = cJSON_GetObjectItemCaseSensitive(d, "pets"), *tasks = cJSON_GetObjectItemCaseSensitive(d, "tasks");
    int id = life_number(c, "id", 0), pet_id = life_number(c, "pet", 0);
    cJSON *pet = find(pets, pet_id);
    if (!strcmp(op, "pet.add") || !strcmp(op, "pet.edit"))
    {
        bool add = !strcmp(op, "pet.add");
        if (add && cJSON_GetArraySize(pets) >= 8)
            return fail(e, n, "Massimo 8 profili");
        cJSON *p = add ? cJSON_CreateObject() : find(pets, id);
        if (!p)
            return fail(e, n, "Profilo non trovato");
        if (add)
        {
            number(p, "id", next_id(d));
            cJSON_AddItemToArray(pets, p);
            text(p, "sex", "unknown");
        }
        if (!field(p, c, "name", 40, add, e, n))
            return false;
        const char *keys[] = {"breed", "relation",  "birth", "sex",       "vet",
                              "phone", "microchip", "food",  "allergies", "notes"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!field(p, c, keys[i],
                       !strcmp(keys[i], "notes")   ? 500
                       : !strcmp(keys[i], "birth") ? 10
                                                   : 120,
                       false, e, n))
                return false;
        const char *birth = life_string(p, "birth");
        if (birth[0] && (life_day(birth) == LIFE_NO_DAY || life_day(birth) > today))
            return fail(e, n, "Nascita non valida o futura");
        const char *sex = life_string(p, "sex");
        if (strcmp(sex, "female") && strcmp(sex, "male") && strcmp(sex, "unknown"))
            return fail(e, n, "Sesso non valido");
        if (cJSON_HasObjectItem(c, "neutered"))
            boolean(p, "neutered", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "neutered")));
        return true;
    }
    if (!strcmp(op, "pet.delete"))
    {
        if (!find(pets, id))
            return fail(e, n, "Profilo non trovato");
        erase_id(pets, id);
        erase_pet(tasks, id);
        erase_pet(cJSON_GetObjectItemCaseSensitive(d, "weights"), id);
        erase_pet(cJSON_GetObjectItemCaseSensitive(d, "logs"), id);
        return true;
    }
    if (!strcmp(op, "task.add") || !strcmp(op, "task.edit"))
    {
        bool add = !strcmp(op, "task.add");
        if (add && cJSON_GetArraySize(tasks) >= 64)
            return fail(e, n, "Massimo 64 attivita");
        cJSON *t = add ? cJSON_CreateObject() : find(tasks, id);
        if (!t)
            return fail(e, n, "Attivita non trovata");
        if (add)
        {
            if (!pet)
            {
                cJSON_Delete(t);
                return fail(e, n, "Scegli un profilo");
            }
            number(t, "id", next_id(d));
            number(t, "pet", pet_id);
            text(t, "kind", "routine");
            boolean(t, "enabled", true);
            boolean(t, "notify", false);
            cJSON_AddItemToArray(tasks, t);
        }
        if (!field(t, c, "label", 80, add, e, n) || !field(t, c, "kind", 24, false, e, n) ||
            !field(t, c, "notes", 500, false, e, n))
            return false;
        const char *date = cJSON_HasObjectItem(c, "due") ? life_string(c, "due") : life_string(t, "due");
        if (life_day(date) == LIFE_NO_DAY)
            return fail(e, n, "Scadenza non valida");
        text(t, "due", date);
        int repeat = life_number(c, "repeat_days", life_number(t, "repeat_days", 0));
        int minute = life_number(c, "minute", life_number(t, "minute", 540));
        if (repeat < 0 || repeat > 3650 || minute < 0 || minute > 1439)
            return fail(e, n, "Ora o ricorrenza non valida");
        number(t, "repeat_days", repeat);
        number(t, "minute", minute);
        if (cJSON_HasObjectItem(c, "enabled"))
            boolean(t, "enabled", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "enabled")));
        if (cJSON_HasObjectItem(c, "notify"))
            boolean(t, "notify", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "notify")));
        return true;
    }
    if (!strcmp(op, "task.delete"))
    {
        erase_id(tasks, id);
        return true;
    }
    if (!strcmp(op, "task.done") || !strcmp(op, "task.postpone"))
    {
        cJSON *t = find(tasks, id);
        if (!t)
            return fail(e, n, "Attivita non trovata");
        int due = life_day(life_string(t, "due"));
        char date[11];
        life_date(today, date);
        if (!strcmp(op, "task.postpone"))
        {
            life_date((due > today ? due : today) + 1, date);
            text(t, "due", date);
            return true;
        }
        if (!strcmp(life_string(t, "last_done"), date))
            return fail(e, n, "Gia registrata oggi");
        if (strlen(life_string(c, "note")) > 500)
            return fail(e, n, "Nota troppo lunga");
        cJSON *logs = cJSON_GetObjectItemCaseSensitive(d, "logs");
        if (cJSON_GetArraySize(logs) >= 1024)
            return fail(e, n, "Archivio pieno: esporta e rimuovi una voce");
        cJSON *log = cJSON_CreateObject();
        number(log, "id", next_id(d));
        number(log, "pet", life_number(t, "pet", 0));
        number(log, "task", id);
        text(log, "date", date);
        text(log, "label", life_string(t, "label"));
        text(log, "kind", life_string(t, "kind"));
        text(log, "note", life_string(c, "note"));
        cJSON_AddItemToArray(logs, log);
        text(t, "last_done", date);
        int repeat = life_number(t, "repeat_days", 0);
        if (repeat)
        {
            life_date((due > today ? due : today) + repeat, date);
            text(t, "due", date);
        }
        else
            boolean(t, "enabled", false);
        return true;
    }
    if (!strcmp(op, "weight.add") || !strcmp(op, "weight.edit"))
    {
        if (!pet)
            return fail(e, n, "Profilo non trovato");
        int grams = life_number(c, "grams", 0);
        int day = life_day(life_string(c, "date"));
        if (grams < 100 || grams > 150000 || day == LIFE_NO_DAY || day > today)
            return fail(e, n, "Peso o data non validi");
        cJSON *a = cJSON_GetObjectItemCaseSensitive(d, "weights");
        bool add = !strcmp(op, "weight.add");
        if (add && cJSON_GetArraySize(a) >= 256)
            return fail(e, n, "Archivio pesi pieno: esporta e rimuovi una misura");
        cJSON *w = add ? cJSON_CreateObject() : find(a, id);
        if (!w)
            return fail(e, n, "Misura non trovata");
        if (add)
        {
            number(w, "id", next_id(d));
            cJSON_AddItemToArray(a, w);
        }
        number(w, "pet", pet_id);
        number(w, "grams", grams);
        text(w, "date", life_string(c, "date"));
        return true;
    }
    if (!strcmp(op, "weight.delete"))
    {
        erase_id(cJSON_GetObjectItemCaseSensitive(d, "weights"), id);
        return true;
    }
    if (!strcmp(op, "log.add") || !strcmp(op, "log.edit"))
    {
        if (!pet)
            return fail(e, n, "Profilo non trovato");
        if (life_day(life_string(c, "date")) == LIFE_NO_DAY)
            return fail(e, n, "Data non valida");
        cJSON *a = cJSON_GetObjectItemCaseSensitive(d, "logs");
        bool add = !strcmp(op, "log.add");
        if (add && cJSON_GetArraySize(a) >= 1024)
            return fail(e, n, "Archivio pieno: esporta e rimuovi una voce");
        cJSON *v = add ? cJSON_CreateObject() : find(a, id);
        if (!v)
            return fail(e, n, "Voce non trovata");
        if (add)
        {
            number(v, "id", next_id(d));
            cJSON_AddItemToArray(a, v);
        }
        number(v, "pet", pet_id);
        if (!field(v, c, "label", 80, true, e, n) || !field(v, c, "note", 500, false, e, n) ||
            !field(v, c, "kind", 24, false, e, n))
        {
            return false;
        }
        text(v, "date", life_string(c, "date"));
        return true;
    }
    if (!strcmp(op, "log.delete"))
    {
        erase_id(cJSON_GetObjectItemCaseSensitive(d, "logs"), id);
        return true;
    }
    return fail(e, n, "Comando non riconosciuto");
}
bool life_apply(life_kind_t kind, cJSON *d, const cJSON *c, int today, char *e, size_t n)
{
    if ((unsigned)kind >= LIFE_COUNT || !cJSON_IsObject(d) || !cJSON_IsObject(c) || today == LIFE_NO_DAY)
        return fail(e, n, "Dati o orologio non validi");
    if (e && n)
        e[0] = 0;
    static const char *numbers[] = {"id",    "city", "preferred", "days",        "every",
                                    "color", "pet",  "minute",    "repeat_days", "grams"};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++)
    {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(c, numbers[i]);
        if (v &&
            (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > INT_MAX || v->valuedouble != v->valueint))
            return fail(e, n, "Numero intero non valido");
    }
    static const char *booleans[] = {"menu_hint", "enabled", "notify", "neutered"};
    for (size_t i = 0; i < sizeof(booleans) / sizeof(booleans[0]); i++)
    {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(c, booleans[i]);
        if (v && !cJSON_IsBool(v))
            return fail(e, n, "Valore si/no non valido");
    }
    return kind == LIFE_RACCOLTA ? waste(d, c, today, e, n) : dogs(d, c, today, e, n);
}
cJSON *life_view(life_kind_t kind, const cJSON *d, int today)
{
    cJSON *out = cJSON_Duplicate(d, true);
    if (!out)
        return NULL;
    cJSON *runtime = cJSON_AddObjectToObject(out, "runtime");
    char date[11];
    life_date(today, date);
    text(runtime, "today", date);
    if (kind == LIFE_RACCOLTA)
    {
        const cJSON *city = life_find(cJSON_GetObjectItemCaseSensitive(d, "cities"), life_number(d, "preferred", 0));
        cJSON *week = cJSON_AddArrayToObject(runtime, "week");
        for (int i = 0; i < 8; i++)
        {
            cJSON *day = cJSON_CreateObject();
            life_date(today + i, date);
            text(day, "date", date);
            number(day, "weekday", life_weekday(today + i));
            cJSON_AddItemToObject(day, "bins", life_collections(city, today + i));
            cJSON_AddItemToArray(week, day);
        }
    }
    else
    {
        int due = 0;
        const cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(
                                  d, "tasks")) if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t, "enabled")) &&
                                                   life_day(life_string(t, "due")) <= today) due++;
        number(runtime, "due", due);
    }
    return out;
}
