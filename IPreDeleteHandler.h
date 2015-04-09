
#ifndef IPRE_DELETE_HANDLER_H
#define IPRE_DELETE_HANDLER_H

class TextBuffer;

struct PreDeleteEvent {
	int pos;
	int nDeleted;
	TextBuffer *buffer;
};

class IPreDeleteHandler {
public:
	virtual ~IPreDeleteHandler() {
	}
	virtual void preDelete(const PreDeleteEvent *event) = 0;
};

#endif
