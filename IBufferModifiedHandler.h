

#ifndef IBUFFER_MODIFIED_HANDLER_H
#define IBUFFER_MODIFIED_HANDLER_H

class TextBuffer;

struct ModifyEvent {
	int pos;
	int nInserted;
	int nDeleted;
	int nRestyled;
	const char *deletedText;
	TextBuffer *buffer;
};

class IBufferModifiedHandler {
public:
	virtual ~IBufferModifiedHandler() {
	}
	virtual void bufferModified(const ModifyEvent *event) = 0;
};

#endif
