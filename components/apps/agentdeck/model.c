#include "model.h"
#include <math.h>
#include <string.h>
static int priority(const cJSON *card)
{
    const cJSON *s=cJSON_GetObjectItemCaseSensitive(card,"status");
    const char *v=cJSON_IsString(s)?s->valuestring:"";
    return !strcmp(v,"needs_input")?0:!strcmp(v,"working")?1:!strcmp(v,"error")?2:
           !strcmp(v,"interrupted")?3:!strcmp(v,"unknown")?4:!strcmp(v,"idle")?5:6;
}
static double observed(const cJSON *card)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(card,"observed_at");
    return cJSON_IsNumber(v)?v->valuedouble:0;
}
int agentdeck_indices(const cJSON *cards,bool all,bool recent,int *out,int capacity)
{
    int count=0;
    for(int i=0;i<cJSON_GetArraySize(cards)&&count<capacity;i++){
        const cJSON *card=cJSON_GetArrayItem(cards,i);
        if(!all&&priority(card)>1)continue;
        int at=count;
        while(at>0){
            const cJSON *previous=cJSON_GetArrayItem(cards,out[at-1]);
            bool before=recent?observed(card)>observed(previous):
                priority(card)<priority(previous)||(priority(card)==priority(previous)&&observed(card)>observed(previous));
            if(!before)break;
            out[at]=out[at-1];at--;
        }
        out[at]=i;count++;
    }
    return count;
}
bool agentdeck_action_allowed(const char *action)
{
    return action&&(!strcmp(action,"verify-projects")||!strcmp(action,"build-amoled"));
}
static bool text(cJSON *to, const cJSON *from, const char *key, unsigned max, bool required)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(from, key);
    if ((!v || cJSON_IsNull(v)) && !required) return true;
    if (!cJSON_IsString(v) || strlen(v->valuestring) > max || (required && !v->valuestring[0])) return false;
    for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++)
        if (*p < 32 && *p != '\n') return false;
    return cJSON_AddStringToObject(to, key, v->valuestring) != NULL;
}
cJSON *agentdeck_validate(const cJSON *input)
{
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(input, "schema");
    const cJSON *cards = cJSON_GetObjectItemCaseSensitive(input, "cards");
    const cJSON *generated = cJSON_GetObjectItemCaseSensitive(input, "generated_at");
    if (!cJSON_IsObject(input) || !cJSON_IsNumber(schema) || schema->valuedouble != 1 ||
        !cJSON_IsArray(cards) || cJSON_GetArraySize(cards) > 16 || !cJSON_IsNumber(generated) ||
        !isfinite(generated->valuedouble) || generated->valuedouble < 0 || generated->valuedouble > 4102444800.0) return NULL;
    cJSON *out = cJSON_CreateObject(), *list = out ? cJSON_AddArrayToObject(out, "cards") : NULL;
    if (!list) { cJSON_Delete(out); return NULL; }
    cJSON_AddNumberToObject(out, "schema", 1);
    cJSON_AddNumberToObject(out, "generated_at", generated->valuedouble);
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(input, "total");
    cJSON_AddNumberToObject(out, "total", cJSON_IsNumber(total) && total->valuedouble >= cJSON_GetArraySize(cards) && total->valuedouble <= 10000 ? total->valuedouble : cJSON_GetArraySize(cards));
    const cJSON *card;
    cJSON_ArrayForEach(card, cards) {
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(list, item)) { cJSON_Delete(item); goto fail; }
        if (!text(item, card, "id", 100, true) || !text(item, card, "name", 100, true) ||
            !text(item, card, "status", 20, true) || !text(item, card, "agent", 16, true) ||
            !text(item, card, "activity", 240, false) || !text(item, card, "repo", 80, false) ||
            !text(item, card, "branch", 100, false) || !text(item, card, "model", 80, false) ||
            !text(item, card, "commit", 40, false) ||
            !text(item, card, "resume", 300, false)) goto fail;
        const char *status = cJSON_GetObjectItemCaseSensitive(item, "status")->valuestring;
        if (strcmp(status,"working") && strcmp(status,"needs_input") && strcmp(status,"done") &&
            strcmp(status,"error") && strcmp(status,"idle") && strcmp(status,"unknown") && strcmp(status,"interrupted")) goto fail;
        for (int i=0; i<cJSON_GetArraySize(list)-1; i++)
            if (!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list,i),"id")->valuestring,
                        cJSON_GetObjectItemCaseSensitive(item,"id")->valuestring)) goto fail;
        const cJSON *context = cJSON_GetObjectItemCaseSensitive(card,"context_percent");
        if (cJSON_IsNumber(context)) {
            if (!isfinite(context->valuedouble) || context->valuedouble < 0 || context->valuedouble > 100) goto fail;
            cJSON_AddNumberToObject(item,"context_percent",context->valuedouble);
        }
        const cJSON *observed = cJSON_GetObjectItemCaseSensitive(card,"observed_at");
        if (cJSON_IsNumber(observed) && isfinite(observed->valuedouble) && observed->valuedouble >= 0 && observed->valuedouble <= 4102444800.0)
            cJSON_AddNumberToObject(item,"observed_at",observed->valuedouble);
        const cJSON *quota=cJSON_GetObjectItemCaseSensitive(card,"quota");
        if(quota && !cJSON_IsNull(quota)) {
            if(!cJSON_IsArray(quota)||cJSON_GetArraySize(quota)>2)goto fail;
            cJSON *limits=cJSON_AddArrayToObject(item,"quota");if(!limits)goto fail;
            const cJSON *q;
            cJSON_ArrayForEach(q,quota){
                const cJSON *remaining=cJSON_GetObjectItemCaseSensitive(q,"remaining_percent");
                if(!cJSON_IsNumber(remaining)||!isfinite(remaining->valuedouble)||remaining->valuedouble<0||remaining->valuedouble>100)goto fail;
                cJSON *v=cJSON_CreateObject();if(!v)goto fail;cJSON_AddItemToArray(limits,v);
                if(!text(v,q,"name",20,true))goto fail;
                cJSON_AddNumberToObject(v,"remaining_percent",remaining->valuedouble);
                const cJSON *reset=cJSON_GetObjectItemCaseSensitive(q,"resets_at");
                if(cJSON_IsNumber(reset)&&isfinite(reset->valuedouble)&&reset->valuedouble>=0&&reset->valuedouble<=4102444800.0)cJSON_AddNumberToObject(v,"resets_at",reset->valuedouble);
            }
        }
    }
    return out;
fail:
    cJSON_Delete(out); return NULL;
}
