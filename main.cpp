
#include "NirvanaQt.h"
#include <QApplication>

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);

	NirvanaQt w;
	w.show();

	return app.exec();
}
