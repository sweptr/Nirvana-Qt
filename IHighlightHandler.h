
#ifndef IHIGHLIGHT_HANDLER_H
#define IHIGHLIGHT_HANDLER_H

class TextBuffer;

struct HighlightEvent {
	int pos;
	TextBuffer *buffer;
};

class IHighlightHandler {
public:
	virtual ~IHighlightHandler() {
	}
	virtual void unfinishedHighlightEncountered(const HighlightEvent *event) = 0;
};

#endif
