
unix {
	CONFIG(debug, debug|release) {
		DEFINES    += QT_SHAREDPOINTER_TRACK_POINTERS
		OBJECTS_DIR = $${OUT_PWD}/.debug/obj
		MOC_DIR     = $${OUT_PWD}/.debug/moc
		RCC_DIR     = $${OUT_PWD}/.debug/rcc
		UI_DIR      = $${OUT_PWD}/.debug/uic
	}
	
	CONFIG(release, debug|release) {
		OBJECTS_DIR = $${OUT_PWD}/.release/obj
		MOC_DIR     = $${OUT_PWD}/.release/moc
		RCC_DIR     = $${OUT_PWD}/.release/rcc
		UI_DIR      = $${OUT_PWD}/.release/uic
	}

	QMAKE_DISTCLEAN += -r       \
		$${OUT_PWD}/.debug/     \
		$${OUT_PWD}/.release/
}


