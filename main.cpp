
#include "QCodeWidget.h"
#include <QApplication>

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);

	QCodeWidget w;
	w.show();

	return app.exec();
}
