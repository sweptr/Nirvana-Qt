greaterThan(QT_MAJOR_VERSION, 4) {
	CONFIG += c++11
} else {
	linux-g++ {
            QMAKE_CXXFLAGS += -std=c++0x
	}
}
