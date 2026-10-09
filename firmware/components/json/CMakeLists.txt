# ESP-IDF 6 moved cJSON out of the core 'json' component into espressif/cjson. BreezyBox still REQUIRES 'json',
# so this shim provides that name and passes cJSON through.
idf_component_register(REQUIRES espressif__cjson)
