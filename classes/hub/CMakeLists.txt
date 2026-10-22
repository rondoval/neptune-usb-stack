# hub.class - root + external hub driver. No GUI. The romtag priority is the shared
# NEP_CLASS_PRI default (-45) and only matters in a Kickstart ROM;
# what makes hubs bind before the device-specific classes is UCCA_Priority in hub.class.c,
# which is a different knob. See add_neptune_class() in the top-level CMakeLists.txt.
# neptune_class_headers: cross-class twin eviction calls the peer hub's usbDoMethodA (inline LVO macros).
add_neptune_class(hub BASETYPE NepHubBase DEPS neptune_class_headers)
add_dependencies(hub_class neptune_class_sfd_headers)
