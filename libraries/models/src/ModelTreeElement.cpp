//
//  ModelTreeElement.cpp
//  libraries/models/src
//
//  Created by Brad Hefta-Gaub on 12/4/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <GeometryUtil.h>

#include "ModelTree.h"
#include "ModelTreeElement.h"

ModelTreeElement::ModelTreeElement(unsigned char* octalCode) : OctreeElement(), _modelItems(NULL) {
    init(octalCode);
};

ModelTreeElement::~ModelTreeElement() {
    _voxelMemoryUsage -= sizeof(ModelTreeElement);
    delete _modelItems;
    _modelItems = NULL;
}

// This will be called primarily on addChildAt(), which means we're adding a child of our
// own type to our own tree. This means we should initialize that child with any tree and type
// specific settings that our children must have. One example is out VoxelSystem, which
// we know must match ours.
OctreeElement* ModelTreeElement::createNewElement(unsigned char* octalCode) {
    ModelTreeElement* newChild = new ModelTreeElement(octalCode);
    newChild->setTree(_myTree);
    return newChild;
}

void ModelTreeElement::init(unsigned char* octalCode) {
    OctreeElement::init(octalCode);
    _modelItems = new QList<ModelItem>;
    _voxelMemoryUsage += sizeof(ModelTreeElement);
}

ModelTreeElement* ModelTreeElement::addChildAtIndex(int index) {
    ModelTreeElement* newElement = (ModelTreeElement*)OctreeElement::addChildAtIndex(index);
    newElement->setTree(_myTree);
    return newElement;
}


bool ModelTreeElement::appendElementData(OctreePacketData* packetData) const {
    bool success = true; // assume the best...

    // write our models out...
    uint16_t numberOfModels = _modelItems->size();
    success = packetData->appendValue(numberOfModels);

    if (success) {
        for (uint16_t i = 0; i < numberOfModels; i++) {
            const ModelItem& model = (*_modelItems)[i];
            success = model.appendModelData(packetData);
            if (!success) {
                break;
            }
        }
    }
    return success;
}

void ModelTreeElement::update(ModelTreeUpdateArgs& args) {
    markWithChangedTime();
    // TODO: early exit when _modelItems is empty

    // update our contained models
    QList<ModelItem>::iterator modelItr = _modelItems->begin();
    while(modelItr != _modelItems->end()) {
        ModelItem& model = (*modelItr);
        model.update(_lastChanged);

        // If the model wants to die, or if it's left our bounding box, then move it
        // into the arguments moving models. These will be added back or deleted completely
        if (model.getShouldDie() || !_box.contains(model.getPosition())) {
            args._movingModels.push_back(model);

            // erase this model
            modelItr = _modelItems->erase(modelItr);
        } else {
            ++modelItr;
        }
    }
    // TODO: if _modelItems is empty after while loop consider freeing memory in _modelItems if
    // internal array is too big (QList internal array does not decrease size except in dtor and
    // assignment operator).  Otherwise _modelItems could become a "resource leak" for large
    // roaming piles of models.
}

bool ModelTreeElement::findSpherePenetration(const glm::vec3& center, float radius,
                                    glm::vec3& penetration, void** penetratedObject) const {
    QList<ModelItem>::iterator modelItr = _modelItems->begin();
    QList<ModelItem>::const_iterator modelEnd = _modelItems->end();
    while(modelItr != modelEnd) {
        ModelItem& model = (*modelItr);
        glm::vec3 modelCenter = model.getPosition();
        float modelRadius = model.getRadius();

        // don't penetrate yourself
        if (modelCenter == center && modelRadius == radius) {
            return false;
        }

        if (findSphereSpherePenetration(center, radius, modelCenter, modelRadius, penetration)) {
            // return true on first valid model penetration
            *penetratedObject = (void*)(&model);
            return true;
        }
        ++modelItr;
    }
    return false;
}

bool ModelTreeElement::updateModel(const ModelItem& model) {
    // NOTE: this method must first lookup the model by ID, hence it is O(N)
    // and "model is not found" is worst-case (full N) but maybe we don't care?
    // (guaranteed that num models per elemen is small?)
    const bool wantDebug = false;
    uint16_t numberOfModels = _modelItems->size();
    for (uint16_t i = 0; i < numberOfModels; i++) {
        ModelItem& thisModel = (*_modelItems)[i];
        if (thisModel.getID() == model.getID()) {
            int difference = thisModel.getLastUpdated() - model.getLastUpdated();
            bool changedOnServer = thisModel.getLastEdited() < model.getLastEdited();
            bool localOlder = thisModel.getLastUpdated() < model.getLastUpdated();
            if (changedOnServer || localOlder) {
                if (wantDebug) {
                    qDebug("local model [id:%d] %s and %s than server model by %d, model.isNewlyCreated()=%s",
                            model.getID(), (changedOnServer ? "CHANGED" : "same"),
                            (localOlder ? "OLDER" : "NEWER"),
                            difference, debug::valueOf(model.isNewlyCreated()) );
                }
                thisModel.copyChangedProperties(model);
                markWithChangedTime();
            } else {
                if (wantDebug) {
                    qDebug(">>> IGNORING SERVER!!! Would've caused jutter! <<<  "
                            "local model [id:%d] %s and %s than server model by %d, model.isNewlyCreated()=%s",
                            model.getID(), (changedOnServer ? "CHANGED" : "same"),
                            (localOlder ? "OLDER" : "NEWER"),
                            difference, debug::valueOf(model.isNewlyCreated()) );
                }
            }
            return true;
        }
    }
    return false;
}

bool ModelTreeElement::updateModel(const ModelItemID& modelID, const ModelItemProperties& properties) {
    uint16_t numberOfModels = _modelItems->size();
    for (uint16_t i = 0; i < numberOfModels; i++) {
        // note: unlike storeModel() which is called from inbound packets, this is only called by local editors
        // and therefore we can be confident that this change is higher priority and should be honored
        ModelItem& thisModel = (*_modelItems)[i];
        
        bool found = false;
        if (modelID.isKnownID) {
            found = thisModel.getID() == modelID.id;
        } else {
            found = thisModel.getCreatorTokenID() == modelID.creatorTokenID;
        }
        if (found) {
            thisModel.setProperties(properties);
            markWithChangedTime(); // mark our element as changed..
            const bool wantDebug = false;
            if (wantDebug) {
                uint64_t now = usecTimestampNow();
                int elapsed = now - thisModel.getLastEdited();

                qDebug() << "ModelTreeElement::updateModel() AFTER update... edited AGO=" << elapsed <<
                        "now=" << now << " thisModel.getLastEdited()=" << thisModel.getLastEdited();
            }                
            return true;
        }
    }
    return false;
}

void ModelTreeElement::updateModelItemID(FindAndUpdateModelItemIDArgs* args) {
    uint16_t numberOfModels = _modelItems->size();
    for (uint16_t i = 0; i < numberOfModels; i++) {
        ModelItem& thisModel = (*_modelItems)[i];
        
        if (!args->creatorTokenFound) {
            // first, we're looking for matching creatorTokenIDs, if we find that, then we fix it to know the actual ID
            if (thisModel.getCreatorTokenID() == args->creatorTokenID) {
                thisModel.setID(args->modelID);
                args->creatorTokenFound = true;
            }
        }
        
        // if we're in an isViewing tree, we also need to look for an kill any viewed models
        if (!args->viewedModelFound && args->isViewing) {
            if (thisModel.getCreatorTokenID() == UNKNOWN_MODEL_TOKEN && thisModel.getID() == args->modelID) {
                _modelItems->removeAt(i); // remove the model at this index
                numberOfModels--; // this means we have 1 fewer model in this list
                i--; // and we actually want to back up i as well.
                args->viewedModelFound = true;
            }
        }
    }
}



const ModelItem* ModelTreeElement::getClosestModel(glm::vec3 position) const {
    const ModelItem* closestModel = NULL;
    float closestModelDistance = FLT_MAX;
    uint16_t numberOfModels = _modelItems->size();
    for (uint16_t i = 0; i < numberOfModels; i++) {
        float distanceToModel = glm::distance(position, (*_modelItems)[i].getPosition());
        if (distanceToModel < closestModelDistance) {
            closestModel = &(*_modelItems)[i];
        }
    }
    return closestModel;
}

void ModelTreeElement::getModels(const glm::vec3& searchPosition, float searchRadius, QVector<const ModelItem*>& foundModels) const {
    uint16_t numberOfModels = _modelItems->size();
    for (uint16_t i = 0; i < numberOfModels; i++) {
        const ModelItem* model = &(*_modelItems)[i];
        float distance = glm::length(model->getPosition() - searchPosition);
        if (distance < searchRadius + model->getRadius()) {
            foundModels.push_back(model);
        }
    }
}

void ModelTreeElement::getModelsForUpdate(const AABox& box, QVector<ModelItem*>& foundModels) {
    QList<ModelItem>::iterator modelItr = _modelItems->begin();
    QList<ModelItem>::iterator modelEnd = _modelItems->end();
    AABox modelBox;
    while(modelItr != modelEnd) {
        ModelItem* model = &(*modelItr);
        float radius = model->getRadius();
        // NOTE: we actually do box-box collision queries here, which is sloppy but good enough for now
        // TODO: decide whether to replace modelBox-box query with sphere-box (requires a square root
        // but will be slightly more accurate).
        modelBox.setBox(model->getPosition() - glm::vec3(radius), 2.f * radius);
        if (modelBox.touches(_box)) {
            foundModels.push_back(model);
        }
        ++modelItr;
    }
}

const ModelItem* ModelTreeElement::getModelWithID(uint32_t id) const {
    // NOTE: this lookup is O(N) but maybe we don't care? (guaranteed that num models per elemen is small?)
    const ModelItem* foundModel = NULL;
    uint16_t numberOfModels = _modelItems->size();
    for (uint16_t i = 0; i < numberOfModels; i++) {
        if ((*_modelItems)[i].getID() == id) {
            foundModel = &(*_modelItems)[i];
            break;
        }
    }
    return foundModel;
}

bool ModelTreeElement::removeModelWithID(uint32_t id) {
    bool foundModel = false;
    uint16_t numberOfModels = _modelItems->size();
    for (uint16_t i = 0; i < numberOfModels; i++) {
        if ((*_modelItems)[i].getID() == id) {
            foundModel = true;
            _modelItems->removeAt(i);
            break;
        }
    }
    return foundModel;
}

int ModelTreeElement::readElementDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
            ReadBitstreamToTreeParams& args) {

    const unsigned char* dataAt = data;
    int bytesRead = 0;
    uint16_t numberOfModels = 0;
    int expectedBytesPerModel = ModelItem::expectedBytes();

    if (bytesLeftToRead >= (int)sizeof(numberOfModels)) {
        // read our models in....
        numberOfModels = *(uint16_t*)dataAt;
        dataAt += sizeof(numberOfModels);
        bytesLeftToRead -= (int)sizeof(numberOfModels);
        bytesRead += sizeof(numberOfModels);

        if (bytesLeftToRead >= (int)(numberOfModels * expectedBytesPerModel)) {
            for (uint16_t i = 0; i < numberOfModels; i++) {
                ModelItem tempModel;
                int bytesForThisModel = tempModel.readModelDataFromBuffer(dataAt, bytesLeftToRead, args);
                _myTree->storeModel(tempModel);
                dataAt += bytesForThisModel;
                bytesLeftToRead -= bytesForThisModel;
                bytesRead += bytesForThisModel;
            }
        }
    }

    return bytesRead;
}

// will average a "common reduced LOD view" from the the child elements...
void ModelTreeElement::calculateAverageFromChildren() {
    // nothing to do here yet...
}

// will detect if children are leaves AND collapsable into the parent node
// and in that case will collapse children and make this node
// a leaf, returns TRUE if all the leaves are collapsed into a
// single node
bool ModelTreeElement::collapseChildren() {
    // nothing to do here yet...
    return false;
}


void ModelTreeElement::storeModel(const ModelItem& model) {
    _modelItems->push_back(model);
    markWithChangedTime();
}

