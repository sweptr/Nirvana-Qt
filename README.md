# Nirvana-Qt

Nedit is a fantastic editor. Unfortunately it being Motif based, it is starting to show its age. This widget is an attempt to port the original text widget code to Qt.
The intention is to be as true to the original as possible, while taking advantage of modern C++11 coding techniques. This should result in much cleaner, simpler code since we can take advantage of things like reusable data structures and Qt's native painting system.

At the moment, it has the same limitations as nedit. For example:

* No Unicode support
* Hand coded regex engine
* Based on `'\0'` terminated strings

Initially, we've inhertited these limitations, but once the code base is proven, things will be refactored to eliminate these limitations.
