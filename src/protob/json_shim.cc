#include <inttypes.h>
#include "protob/json_shim.hpp"

#include "utils.hpp"
#include "debug.hpp"

#include "http/json.hpp"
#include "rdb_protocol/ql2.pb.h"

std::map<std::string, int32_t> resolver;

namespace json_shim {

class exc_t : public std::exception {
public:
    exc_t(cJSON *) : str("PLACEHOLDER") { BREAKPOINT; }
    ~exc_t() throw () { }
    const char *what() const throw () { return str.c_str(); }
private:
    std::string str;
};

void check_type(cJSON *json, int expected) {
    if (json->type != expected) throw exc_t(json);
}

// RSI: parenthesize
#define DEFINE_TRANSFER(FIELD, NAME)                                    \
struct FIELD##_meta_extractor__{                                        \
    template<class T__, class = void>                                   \
    struct t__;                                                         \
    template<class T__>                                                 \
    struct t__<T__, typename                                            \
               std::enable_if<&T__::set_##FIELD != NULL>::type> {       \
        void operator()(cJSON *json__, T__ *dest__) {                   \
            decltype(dest__->FIELD()) tmp__;                            \
            cJSON *item__ = cJSON_GetObjectItem(json__, NAME);          \
            if (item__ == NULL) return;                                 \
            safe_extractor_t<decltype(tmp__)>()(item__, &tmp__);        \
            dest__->set_##FIELD(std::move(tmp__));                      \
        }                                                               \
    };                                                                  \
    template<class T__>                                                 \
    struct t__<T__, typename                                            \
               std::enable_if<&T__::release_##FIELD != NULL>::type> {   \
        void operator()(cJSON *json__, T__ *dest__) {                   \
            cJSON *item__ = cJSON_GetObjectItem(json__, NAME);          \
            if (item__ == NULL) return;                                 \
            safe_extractor_t<decltype(dest__->FIELD())>()(              \
                item__, dest__->mutable_##FIELD());                     \
        }                                                               \
    };                                                                  \
    template<class T__>                                                 \
    struct t__<T__, typename                                            \
               std::enable_if<&T__::FIELD##_size != NULL>::type> {      \
        void operator()(cJSON *json__, T__ *dest__) {                   \
            cJSON *arr__ = cJSON_GetObjectItem(json__, NAME);           \
            if (arr__ == NULL) return;                                  \
            if (arr__->type != cJSON_Object) {                          \
                check_type(arr__, cJSON_Array);                         \
            }                                                           \
            int sz__ = cJSON_GetArraySize(arr__);                       \
            for (int i__ = 0; i__ < sz__; ++i__) {                      \
                auto el__ = dest__->add_##FIELD();                      \
                safe_extractor_t<decltype(dest__->FIELD(0))>()(         \
                    cJSON_GetArrayItem(arr__, i__),                     \
                    el__);                                              \
            }                                                           \
        }                                                               \
    };                                                                  \
    template<class T>                                                   \
    void operator()(cJSON *json__, T *dest__) {                         \
        t__<T>()(json__, dest__);                                       \
    }                                                                   \
};                                                                      \
template<class T>                                                       \
void transfer_##FIELD(cJSON *json__, T *dest__) {                       \
    FIELD##_meta_extractor__ meta_extractor__;                          \
    meta_extractor__(json__, dest__);                                   \
}                                                                       \
struct FIELD##_force_semicolon { }

template<class T, class = void>
struct extractor_t;

template<class T>
struct safe_extractor_t : public extractor_t<T> {
    template<class U>
    void operator()(cJSON *json, U *t) {
        if (json != NULL && t != NULL) {
            (*static_cast<extractor_t<T> *>(this))(json, t);
        }
    }
};

DEFINE_TRANSFER(type, "t");
DEFINE_TRANSFER(query, "q");
DEFINE_TRANSFER(token, "k");
DEFINE_TRANSFER(datum, "d");
DEFINE_TRANSFER(args, "s");
DEFINE_TRANSFER(optargs, "o");
DEFINE_TRANSFER(global_optargs, "g");

template<class T>
struct extractor_t<
    T, typename std::enable_if<std::is_enum<T>::value
                               || std::is_fundamental<T>::value>::type> {
    void operator()(cJSON *field, T *dest) {
        if (field->type != cJSON_Number) throw exc_t(field);
        T t = static_cast<T>(field->valuedouble);
        if (static_cast<double>(t) != field->valuedouble) throw exc_t(field);
        *dest = t;
    }
};

template<>
struct extractor_t<const std::string &> {
    void operator()(cJSON *field, std::string *s) {
        check_type(field, cJSON_String);
        *s = field->valuestring;
    };
};

template<>
struct extractor_t<bool> {
    void operator()(cJSON *field, bool *dest) {
        if (field->type == cJSON_False) {
            *dest = false;
        } else if (field->type == cJSON_True) {
            *dest = true;
        } else {
            throw exc_t(field);
        }
    }
};

template<>
struct extractor_t<const Term &> {
    void operator()(cJSON *json, Term *t) {
        transfer_type(json, t);
        transfer_datum(json, t);
        transfer_args(json, t);
        transfer_optargs(json, t);
    }
};

template<>
struct extractor_t<const Datum &> {
    void operator()(cJSON *json, Datum *d) {
        switch(json->type) {
        case cJSON_False: {
            d->set_type(Datum::R_BOOL);
            d->set_r_bool(false);
        } break;
        case cJSON_True: {
            d->set_type(Datum::R_BOOL);
            d->set_r_bool(true);
        } break;
        case cJSON_NULL: {
            d->set_type(Datum::R_NULL);
        } break;
        case cJSON_Number: {
            d->set_type(Datum::R_NUM);
            d->set_r_num(json->valuedouble);
        } break;
        case cJSON_String: {
            d->set_type(Datum::R_STR);
            d->set_r_str(json->valuestring);
        } break;
        case cJSON_Array: {
            d->set_type(Datum::R_ARRAY);
            int sz = cJSON_GetArraySize(json);
            for (int i = 0; i < sz; ++i) {
                cJSON *item = cJSON_GetArrayItem(json, i);
                (*this)(item, d->add_r_array());
            }
        } break;
        case cJSON_Object: {
            d->set_type(Datum::R_OBJECT);
            int sz = cJSON_GetArraySize(json);
            for (int i = 0; i < sz; ++i) {
                cJSON *item = cJSON_GetArrayItem(json, i);
                Datum::AssocPair *ap = d->add_r_object();
                ap->set_key(item->string);
                (*this)(item, ap->mutable_val());
            }
        } break;
        default: unreachable();
        }
    }
};

template<>
struct extractor_t<const Query::AssocPair &> {
    void operator()(cJSON *json, Query::AssocPair *ap) {
        ap->set_key(json->string);
        extractor_t<const Term &>()(json, ap->mutable_val());
    };
};

template<>
struct extractor_t<const Term::AssocPair &> {
    void operator()(cJSON *json, Term::AssocPair *ap) {
        ap->set_key(json->string);
        extractor_t<const Term &>()(json, ap->mutable_val());
    };
};

template<>
struct extractor_t<const Datum::AssocPair &> {
    void operator()(cJSON *json, Datum::AssocPair *ap) {
        ap->set_key(json->string);
        extractor_t<const Datum &>()(json, ap->mutable_val());
    };
};

template<>
struct extractor_t<const Query &> {
    void operator()(cJSON *json, Query *q) {
        transfer_type(json, q);
        transfer_query(json, q);
        transfer_token(json, q);
        q->set_accepts_r_json(true);
        transfer_global_optargs(json, q);
    };
};

bool parse_json_pb(Query *q, char *str) throw () {
    // debug_timer_t tm;
    q->Clear();
    scoped_cJSON_t json_holder(cJSON_Parse(str));
    // tm.tick("JSON");
    cJSON *json = json_holder.get();
    if (json == NULL) return false;
    extractor_t<const Query &>()(json, q);
    // tm.tick("SHIM");
    // debugf("%s\n", q->DebugString().c_str());
    return true;
}

int64_t write_json_pb(const Response *r, std::string *s) throw () {
    *s += strprintf("{\"t\":%d,\"k\":%" PRIi64 ",\"r\":[", r->type(), r->token());
    for (int i = 0; i < r->response_size(); ++i) {
        *s += (i == 0) ? "" : ",";
        const Datum *d = &r->response(i);
        if (d->type() == Datum::R_JSON) {
            *s += d->r_str();
        } else if (d->type() == Datum::R_STR) {
            scoped_cJSON_t tmp(cJSON_CreateString(d->r_str().c_str()));
            *s += tmp.PrintUnformatted();
        } else {
            unreachable();
        }
    }
    *s += "]";

    if (r->has_backtrace()) {
        *s += ",\"b\":";
        const Backtrace *bt = &r->backtrace();
        scoped_cJSON_t arr(cJSON_CreateArray());
        for (int i = 0; i < bt->frames_size(); ++i) {
            const Frame *f = &bt->frames(i);
            switch (f->type()) {
            case Frame::POS: {
                arr.AddItemToArray(cJSON_CreateNumber(f->pos()));
            } break;
            case Frame::OPT: {
                arr.AddItemToArray(cJSON_CreateString(f->opt().c_str()));
            } break;
            default: unreachable();
            }
        }
        *s += arr.PrintUnformatted();
    }

    if (r->has_profile()) {
        *s += ",\"p\":";
        const Datum *d = &r->profile();
        guarantee(d->type() == Datum::R_JSON);
        *s += d->r_str();
    }

    *s += "}";
    return r->token();
}


} // namespace json_shim