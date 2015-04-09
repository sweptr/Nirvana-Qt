
#ifndef IHIGHLIGHT_HANDLER_H
#define IHIGHLIGHT_HANDLER_H

class IHighlightHandler {
public:
	virtual ~IHighlightHandler() {
	}
	virtual void unfinishedHighlightEncountered(int pos) = 0;
};

#endif
