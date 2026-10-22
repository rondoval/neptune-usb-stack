# hubss.class - USB 3.0 SuperSpeed hub (AROS-native rewrite of hub.class). No GUI; hub-like.
# Struct header is hubss.class.h (NepHubSSBase/NepClassHubSS), so INCLUDE points there.
# neptune_class_headers: cross-class twin eviction calls the peer hub's usbDoMethodA (inline LVO macros).
add_neptune_class(hubss BASETYPE NepHubSSBase INCLUDE hubss.class.h DEPS neptune_class_headers)
add_dependencies(hubss_class neptune_class_sfd_headers)
