
#ifndef ICURSORMOVE_HANDLER_H
#define ICURSORMOVE_HANDLER_H

class ICursorMoveHandler {
public:
	virtual ~ICursorMoveHandler() {
	}
	virtual void cursorMoved() = 0;
};

#endif
