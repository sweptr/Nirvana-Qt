
#ifndef SELECTION_H_
#define SELECTION_H_

class Selection {
public:
	Selection();
	Selection(const Selection &) = default;
	Selection &operator=(const Selection &) = default;

public:
	bool selected;    // true if the Selection is active
	bool rectangular; // true if the Selection is rectangular
	bool zeroWidth;   // Width 0 selections aren't "real" selections, but they can
	                  // be useful when creating rectangular selections from the
	                  // keyboard.
	int start;        // Pos. of start of Selection, or if rectangular start of line
	                  // containing it.
	int end;          // Pos. of end of Selection, or if rectangular end of line containing
	                  // it.
	int rectStart;    // Indent of left edge of rect. Selection
	int rectEnd;      // Indent of right edge of rect. Selection
};

#endif
