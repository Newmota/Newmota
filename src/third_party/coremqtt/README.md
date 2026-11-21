# coreMQTT serializer

This directory contains the unmodified MQTT 3.1.1 serializer/deserializer
layer from FreeRTOS coreMQTT v2.3.1.

- Upstream: <https://github.com/FreeRTOS/coreMQTT>
- Pinned tag: `v2.3.1`
- Pinned source commit: `2beef04725328923e05e576b884212d53ec97af7`
- License: MIT, see [LICENSE](LICENSE)

Newmota imports only `core_mqtt_serializer.c`, its public serializer header,
the default configuration header, and `transport_interface.h`. The managed
coreMQTT client and state machine are deliberately not imported. Newmota owns
the connection lifecycle, bounded packet framing, reconnect policy, TLS, topic
policy, settings, and canonical-message routing.

The imported source is compiled with `MQTT_DO_NOT_USE_CUSTOM_CONFIG=1` until a
Newmota-specific coreMQTT configuration is needed. Do not edit the imported
files; place adaptations in `src/app/services/mqtt/`.
