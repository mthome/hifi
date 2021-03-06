//
//  OctreePersistThread.h
//  libraries/octree/src
//
//  Created by Brad Hefta-Gaub on 8/21/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Threaded or non-threaded Octree persistence
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_OctreePersistThread_h
#define hifi_OctreePersistThread_h

#include <QString>
#include <GenericThread.h>
#include "Octree.h"

/// Generalized threaded processor for handling received inbound packets.
class OctreePersistThread : public GenericThread {
    Q_OBJECT
public:
    static const int DEFAULT_PERSIST_INTERVAL = 1000 * 30; // every 30 seconds

    OctreePersistThread(Octree* tree, const QString& filename, int persistInterval = DEFAULT_PERSIST_INTERVAL);

    bool isInitialLoadComplete() const { return _initialLoadComplete; }
    quint64 getLoadElapsedTime() const { return _loadTimeUSecs; }

signals:
    void loadCompleted();

protected:
    /// Implements generic processing behavior for this thread.
    virtual bool process();
private:
    Octree* _tree;
    QString _filename;
    int _persistInterval;
    bool _initialLoadComplete;

    quint64 _loadTimeUSecs;
    quint64 _lastCheck;
};

#endif // hifi_OctreePersistThread_h
