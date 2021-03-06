//
//  MyAvatar.cpp
//  interface/src/avatar
//
//  Created by Mark Peng on 8/16/13.
//  Copyright 2012 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <algorithm>
#include <vector>

#include <QMessageBox>
#include <QBuffer>

#include <glm/gtx/norm.hpp>
#include <glm/gtx/vector_angle.hpp>

#include <QtCore/QTimer>

#include <AccountManager.h>
#include <GeometryUtil.h>
#include <NodeList.h>
#include <PacketHeaders.h>
#include <SharedUtil.h>

#include <ShapeCollider.h>

#include "Application.h"
#include "Audio.h"
#include "Environment.h"
#include "Menu.h"
#include "MyAvatar.h"
#include "Physics.h"
#include "devices/Faceshift.h"
#include "devices/OculusManager.h"
#include "ui/TextRenderer.h"

using namespace std;

const glm::vec3 DEFAULT_UP_DIRECTION(0.0f, 1.0f, 0.0f);
const float YAW_SPEED = 500.0f;   // degrees/sec
const float PITCH_SPEED = 100.0f; // degrees/sec
const float COLLISION_RADIUS_SCALAR = 1.2f; // pertains to avatar-to-avatar collisions
const float COLLISION_RADIUS_SCALE = 0.125f;

const float DATA_SERVER_LOCATION_CHANGE_UPDATE_MSECS = 5.0f * 1000.0f;

// TODO: normalize avatar speed for standard avatar size, then scale all motion logic 
// to properly follow avatar size.
float DEFAULT_MOTOR_TIMESCALE = 0.25f;
float MAX_AVATAR_SPEED = 300.0f;
float MAX_MOTOR_SPEED = MAX_AVATAR_SPEED; 

MyAvatar::MyAvatar() :
	Avatar(),
    _mousePressed(false),
    _bodyPitchDelta(0.0f),
    _bodyRollDelta(0.0f),
    _shouldJump(false),
    _gravity(0.0f, 0.0f, 0.0f),
    _distanceToNearestAvatar(std::numeric_limits<float>::max()),
    _wasPushing(false),
    _isPushing(false),
    _wasStuck(false),
    _thrust(0.0f),
    _motorVelocity(0.0f),
    _motorTimescale(DEFAULT_MOTOR_TIMESCALE),
    _maxMotorSpeed(MAX_MOTOR_SPEED),
    _motionBehaviors(AVATAR_MOTION_DEFAULTS),
    _lastBodyPenetration(0.0f),
    _lastFloorContactPoint(0.0f),
    _lookAtTargetAvatar(),
    _shouldRender(true),
    _billboardValid(false),
    _oculusYawOffset(0.0f)
{
    for (int i = 0; i < MAX_DRIVE_KEYS; i++) {
        _driveKeys[i] = 0.0f;
    }
    
    // update our location every 5 seconds in the data-server, assuming that we are authenticated with one
    QTimer* locationUpdateTimer = new QTimer(this);
    connect(locationUpdateTimer, &QTimer::timeout, this, &MyAvatar::updateLocationInDataServer);
    locationUpdateTimer->start(DATA_SERVER_LOCATION_CHANGE_UPDATE_MSECS);
}

MyAvatar::~MyAvatar() {
    _lookAtTargetAvatar.clear();
}

void MyAvatar::reset() {
    _skeletonModel.reset();
    getHead()->reset(); 
    getHand()->reset();
    _oculusYawOffset = 0.0f;

    setVelocity(glm::vec3(0.0f));
    setThrust(glm::vec3(0.0f));
    setOrientation(glm::quat(glm::vec3(0.0f)));
}

void MyAvatar::update(float deltaTime) {
    Head* head = getHead();
    head->relaxLean(deltaTime);
    updateFromGyros(deltaTime);
    if (Menu::getInstance()->isOptionChecked(MenuOption::MoveWithLean)) {
        // Faceshift drive is enabled, set the avatar drive based on the head position
        moveWithLean();
    }
    
    //  Get audio loudness data from audio input device
    Audio* audio = Application::getInstance()->getAudio();
    head->setAudioLoudness(audio->getLastInputLoudness());
    head->setAudioAverageLoudness(audio->getAudioAverageInputLoudness());

    if (_motionBehaviors & AVATAR_MOTION_OBEY_ENVIRONMENTAL_GRAVITY) {
        setGravity(Application::getInstance()->getEnvironment()->getGravity(getPosition()));
    }

    simulate(deltaTime);
}

void MyAvatar::simulate(float deltaTime) {

    if (_scale != _targetScale) {
        float scale = (1.0f - SMOOTHING_RATIO) * _scale + SMOOTHING_RATIO * _targetScale;
        setScale(scale);
        Application::getInstance()->getCamera()->setScale(scale);
    }

    // update the movement of the hand and process handshaking with other avatars...
    updateHandMovementAndTouching(deltaTime);

    updateOrientation(deltaTime);

    float keyboardInput = fabsf(_driveKeys[FWD] - _driveKeys[BACK]) + 
        fabsf(_driveKeys[RIGHT] - _driveKeys[LEFT]) + 
        fabsf(_driveKeys[UP] - _driveKeys[DOWN]);

    bool walkingOnFloor = false;
    float gravityLength = glm::length(_gravity);
    if (gravityLength > EPSILON) {
        const CapsuleShape& boundingShape = _skeletonModel.getBoundingShape();
        glm::vec3 startCap;
        boundingShape.getStartPoint(startCap);
        glm::vec3 bottomOfBoundingCapsule = startCap + (boundingShape.getRadius() / gravityLength) * _gravity;

        float fallThreshold = 2.0f * deltaTime * gravityLength;
        walkingOnFloor = (glm::distance(bottomOfBoundingCapsule, _lastFloorContactPoint) < fallThreshold);
    }

    if (keyboardInput > 0.0f || glm::length2(_velocity) > 0.0f || glm::length2(_thrust) > 0.0f || 
            ! walkingOnFloor) {
        // apply gravity
        _velocity += _scale * _gravity * (GRAVITY_EARTH * deltaTime);
    
        // update motor and thrust
        updateMotorFromKeyboard(deltaTime, walkingOnFloor);
        applyMotor(deltaTime);
        applyThrust(deltaTime);

        // update position
        if (glm::length2(_velocity) < EPSILON) {
            _velocity = glm::vec3(0.0f);
        } else { 
            _position += _velocity * deltaTime;
        }
    }

    // update moving flag based on speed
    const float MOVING_SPEED_THRESHOLD = 0.01f;
    _moving = glm::length(_velocity) > MOVING_SPEED_THRESHOLD;
    updateChatCircle(deltaTime);

    // update avatar skeleton and simulate hand and head
    getHand()->collideAgainstOurself(); 
    getHand()->simulate(deltaTime, true);

    _skeletonModel.simulate(deltaTime);
    simulateAttachments(deltaTime);

    // copy out the skeleton joints from the model
    _jointData.resize(_skeletonModel.getJointStateCount());
    for (int i = 0; i < _jointData.size(); i++) {
        JointData& data = _jointData[i];
        data.valid = _skeletonModel.getJointState(i, data.rotation);
    }

    Head* head = getHead();
    glm::vec3 headPosition;
    if (!_skeletonModel.getHeadPosition(headPosition)) {
        headPosition = _position;
    }
    head->setPosition(headPosition);
    head->setScale(_scale);
    head->simulate(deltaTime, true);

    // now that we're done stepping the avatar forward in time, compute new collisions
    if (_collisionGroups != 0) {
        Camera* myCamera = Application::getInstance()->getCamera();

        float radius = getSkeletonHeight() * COLLISION_RADIUS_SCALE;
        if (myCamera->getMode() == CAMERA_MODE_FIRST_PERSON && !OculusManager::isConnected()) {
            radius = myCamera->getAspectRatio() * (myCamera->getNearClip() / cos(myCamera->getFieldOfView() / 2.0f));
            radius *= COLLISION_RADIUS_SCALAR;
        }
        if (_collisionGroups) {
            updateShapePositions();
            if (_collisionGroups & COLLISION_GROUP_ENVIRONMENT) {
                updateCollisionWithEnvironment(deltaTime, radius);
            }
            if (_collisionGroups & COLLISION_GROUP_VOXELS) {
                updateCollisionWithVoxels(deltaTime, radius);
            } else {
                _wasStuck = false;
            }
            if (_collisionGroups & COLLISION_GROUP_AVATARS) {
                updateCollisionWithAvatars(deltaTime);
            }
        }
    }

    // consider updating our billboard
    maybeUpdateBillboard();
}

//  Update avatar head rotation with sensor data
void MyAvatar::updateFromGyros(float deltaTime) {
    glm::vec3 estimatedPosition, estimatedRotation;

    FaceTracker* tracker = Application::getInstance()->getActiveFaceTracker();
    if (tracker) {
        estimatedPosition = tracker->getHeadTranslation();
        estimatedRotation = glm::degrees(safeEulerAngles(tracker->getHeadRotation()));
        
        //  Rotate the body if the head is turned beyond the screen
        if (Menu::getInstance()->isOptionChecked(MenuOption::TurnWithHead)) {
            const float TRACKER_YAW_TURN_SENSITIVITY = 0.5f;
            const float TRACKER_MIN_YAW_TURN = 15.0f;
            const float TRACKER_MAX_YAW_TURN = 50.0f;
            if ( (fabs(estimatedRotation.y) > TRACKER_MIN_YAW_TURN) &&
                 (fabs(estimatedRotation.y) < TRACKER_MAX_YAW_TURN) ) {
                if (estimatedRotation.y > 0.0f) {
                    _bodyYawDelta += (estimatedRotation.y - TRACKER_MIN_YAW_TURN) * TRACKER_YAW_TURN_SENSITIVITY;
                } else {
                    _bodyYawDelta += (estimatedRotation.y + TRACKER_MIN_YAW_TURN) * TRACKER_YAW_TURN_SENSITIVITY;
                }
            }
        }
    }

    // Set the rotation of the avatar's head (as seen by others, not affecting view frustum)
    // to be scaled such that when the user's physical head is pointing at edge of screen, the
    // avatar head is at the edge of the in-world view frustum.  So while a real person may move
    // their head only 30 degrees or so, this may correspond to a 90 degree field of view.
    // Note that roll is magnified by a constant because it is not related to field of view.

    float magnifyFieldOfView = Menu::getInstance()->getFieldOfView() / Menu::getInstance()->getRealWorldFieldOfView();
    
    Head* head = getHead();
    head->setDeltaPitch(estimatedRotation.x * magnifyFieldOfView);
    head->setDeltaYaw(estimatedRotation.y * magnifyFieldOfView);
    head->setDeltaRoll(estimatedRotation.z);

    //  Update torso lean distance based on accelerometer data
    const float TORSO_LENGTH = 0.5f;
    glm::vec3 relativePosition = estimatedPosition - glm::vec3(0.0f, -TORSO_LENGTH, 0.0f);
    const float MAX_LEAN = 45.0f;
    head->setLeanSideways(glm::clamp(glm::degrees(atanf(relativePosition.x * _leanScale / TORSO_LENGTH)),
        -MAX_LEAN, MAX_LEAN));
    head->setLeanForward(glm::clamp(glm::degrees(atanf(relativePosition.z * _leanScale / TORSO_LENGTH)),
        -MAX_LEAN, MAX_LEAN));
}

void MyAvatar::moveWithLean() {
    //  Move with Lean by applying thrust proportional to leaning
    Head* head = getHead();
    glm::quat orientation = head->getCameraOrientation();
    glm::vec3 front = orientation * IDENTITY_FRONT;
    glm::vec3 right = orientation * IDENTITY_RIGHT;
    float leanForward = head->getLeanForward();
    float leanSideways = head->getLeanSideways();

    //  Degrees of 'dead zone' when leaning, and amount of acceleration to apply to lean angle
    const float LEAN_FWD_DEAD_ZONE = 15.0f;
    const float LEAN_SIDEWAYS_DEAD_ZONE = 10.0f;
    const float LEAN_FWD_THRUST_SCALE = 4.0f;
    const float LEAN_SIDEWAYS_THRUST_SCALE = 3.0f;

    if (fabs(leanForward) > LEAN_FWD_DEAD_ZONE) {
        if (leanForward > 0.0f) {
            addThrust(front * -(leanForward - LEAN_FWD_DEAD_ZONE) * LEAN_FWD_THRUST_SCALE);
        } else {
            addThrust(front * -(leanForward + LEAN_FWD_DEAD_ZONE) * LEAN_FWD_THRUST_SCALE);
        }
    }
    if (fabs(leanSideways) > LEAN_SIDEWAYS_DEAD_ZONE) {
        if (leanSideways > 0.0f) {
            addThrust(right * -(leanSideways - LEAN_SIDEWAYS_DEAD_ZONE) * LEAN_SIDEWAYS_THRUST_SCALE);
        } else {
            addThrust(right * -(leanSideways + LEAN_SIDEWAYS_DEAD_ZONE) * LEAN_SIDEWAYS_THRUST_SCALE);
        }
    }
}

void MyAvatar::renderDebugBodyPoints() {
    glm::vec3 torsoPosition(getPosition());
    glm::vec3 headPosition(getHead()->getEyePosition());
    float torsoToHead = glm::length(headPosition - torsoPosition);
    glm::vec3 position;
    qDebug("head-above-torso %.2f, scale = %0.2f", torsoToHead, getScale());

    //  Torso Sphere
    position = torsoPosition;
    glPushMatrix();
    glColor4f(0, 1, 0, .5f);
    glTranslatef(position.x, position.y, position.z);
    glutSolidSphere(0.2, 10, 10);
    glPopMatrix();

    //  Head Sphere
    position = headPosition;
    glPushMatrix();
    glColor4f(0, 1, 0, .5f);
    glTranslatef(position.x, position.y, position.z);
    glutSolidSphere(0.15, 10, 10);
    glPopMatrix();
}

// virtual
void MyAvatar::render(const glm::vec3& cameraPosition, RenderMode renderMode) {
    // don't render if we've been asked to disable local rendering
    if (!_shouldRender) {
        return; // exit early
    }
    Avatar::render(cameraPosition, renderMode);
    if (Menu::getInstance()->isOptionChecked(MenuOption::ShowIKConstraints)) {
        _skeletonModel.renderIKConstraints();
    }
}

void MyAvatar::renderHeadMouse(int screenWidth, int screenHeight) const {
    
    Faceshift* faceshift = Application::getInstance()->getFaceshift();
    
    float pixelsPerDegree = screenHeight / Menu::getInstance()->getFieldOfView();
    
    //  Display small target box at center or head mouse target that can also be used to measure LOD
    float headPitch = getHead()->getFinalPitch();
    float headYaw = getHead()->getFinalYaw();

    float aspectRatio = (float) screenWidth / (float) screenHeight;
    int headMouseX = (int)((float)screenWidth / 2.0f - headYaw * aspectRatio * pixelsPerDegree);
    int headMouseY = (int)((float)screenHeight / 2.0f - headPitch * pixelsPerDegree);
    
    glColor3f(1.0f, 1.0f, 1.0f);
    glDisable(GL_LINE_SMOOTH);
    const int PIXEL_BOX = 16;
    glBegin(GL_LINES);
    glVertex2f(headMouseX - PIXEL_BOX/2, headMouseY);
    glVertex2f(headMouseX + PIXEL_BOX/2, headMouseY);
    glVertex2f(headMouseX, headMouseY - PIXEL_BOX/2);
    glVertex2f(headMouseX, headMouseY + PIXEL_BOX/2);
    glEnd();
    glEnable(GL_LINE_SMOOTH);
    //  If Faceshift is active, show eye pitch and yaw as separate pointer
    if (faceshift->isActive()) {

        float avgEyePitch = faceshift->getEstimatedEyePitch();
        float avgEyeYaw = faceshift->getEstimatedEyeYaw();
        int eyeTargetX = (int)((float)(screenWidth) / 2.0f - avgEyeYaw * aspectRatio * pixelsPerDegree);
        int eyeTargetY = (int)((float)(screenHeight) / 2.0f - avgEyePitch * pixelsPerDegree);
        
        glColor3f(0.0f, 1.0f, 1.0f);
        glDisable(GL_LINE_SMOOTH);
        glBegin(GL_LINES);
        glVertex2f(eyeTargetX - PIXEL_BOX/2, eyeTargetY);
        glVertex2f(eyeTargetX + PIXEL_BOX/2, eyeTargetY);
        glVertex2f(eyeTargetX, eyeTargetY - PIXEL_BOX/2);
        glVertex2f(eyeTargetX, eyeTargetY + PIXEL_BOX/2);
        glEnd();

    }
}

void MyAvatar::setLocalGravity(glm::vec3 gravity) {
    _motionBehaviors |= AVATAR_MOTION_OBEY_LOCAL_GRAVITY;
    // Environmental and Local gravities are incompatible.  Since Local is being set here
    // the environmental setting must be removed.
    _motionBehaviors &= ~AVATAR_MOTION_OBEY_ENVIRONMENTAL_GRAVITY;
    setGravity(gravity);
}

void MyAvatar::setGravity(const glm::vec3& gravity) {
    _gravity = gravity;
    getHead()->setGravity(_gravity);

    // use the gravity to determine the new world up direction, if possible
    float gravityLength = glm::length(gravity);
    if (gravityLength > EPSILON) {
        _worldUpDirection = _gravity / -gravityLength;
    } else {
        _worldUpDirection = DEFAULT_UP_DIRECTION;
    }
}

void MyAvatar::saveData(QSettings* settings) {
    settings->beginGroup("Avatar");

    settings->setValue("bodyYaw", _bodyYaw);
    settings->setValue("bodyPitch", _bodyPitch);
    settings->setValue("bodyRoll", _bodyRoll);

    settings->setValue("headPitch", getHead()->getBasePitch());

    settings->setValue("position_x", _position.x);
    settings->setValue("position_y", _position.y);
    settings->setValue("position_z", _position.z);

    settings->setValue("pupilDilation", getHead()->getPupilDilation());

    settings->setValue("leanScale", _leanScale);
    settings->setValue("scale", _targetScale);
    
    settings->setValue("faceModelURL", _faceModelURL);
    settings->setValue("skeletonModelURL", _skeletonModelURL);
    
    settings->beginWriteArray("attachmentData");
    for (int i = 0; i < _attachmentData.size(); i++) {
        settings->setArrayIndex(i);
        const AttachmentData& attachment = _attachmentData.at(i);
        settings->setValue("modelURL", attachment.modelURL);
        settings->setValue("jointName", attachment.jointName);
        settings->setValue("translation_x", attachment.translation.x);
        settings->setValue("translation_y", attachment.translation.y);
        settings->setValue("translation_z", attachment.translation.z);
        glm::vec3 eulers = safeEulerAngles(attachment.rotation);
        settings->setValue("rotation_x", eulers.x);
        settings->setValue("rotation_y", eulers.y);
        settings->setValue("rotation_z", eulers.z);
        settings->setValue("scale", attachment.scale);
    }
    settings->endArray();
    
    settings->setValue("displayName", _displayName);

    settings->endGroup();
}

void MyAvatar::loadData(QSettings* settings) {
    settings->beginGroup("Avatar");

    // in case settings is corrupt or missing loadSetting() will check for NaN
    _bodyYaw = loadSetting(settings, "bodyYaw", 0.0f);
    _bodyPitch = loadSetting(settings, "bodyPitch", 0.0f);
    _bodyRoll = loadSetting(settings, "bodyRoll", 0.0f);

    getHead()->setBasePitch(loadSetting(settings, "headPitch", 0.0f));

    _position.x = loadSetting(settings, "position_x", START_LOCATION.x);
    _position.y = loadSetting(settings, "position_y", START_LOCATION.y);
    _position.z = loadSetting(settings, "position_z", START_LOCATION.z);

    getHead()->setPupilDilation(loadSetting(settings, "pupilDilation", 0.0f));

    _leanScale = loadSetting(settings, "leanScale", 0.05f);
    _targetScale = loadSetting(settings, "scale", 1.0f);
    setScale(_scale);
    Application::getInstance()->getCamera()->setScale(_scale);
    
    setFaceModelURL(settings->value("faceModelURL", DEFAULT_HEAD_MODEL_URL).toUrl());
    setSkeletonModelURL(settings->value("skeletonModelURL").toUrl());
    
    QVector<AttachmentData> attachmentData;
    int attachmentCount = settings->beginReadArray("attachmentData");
    for (int i = 0; i < attachmentCount; i++) {
        settings->setArrayIndex(i);
        AttachmentData attachment;
        attachment.modelURL = settings->value("modelURL").toUrl();
        attachment.jointName = settings->value("jointName").toString();
        attachment.translation.x = loadSetting(settings, "translation_x", 0.0f);
        attachment.translation.y = loadSetting(settings, "translation_y", 0.0f);
        attachment.translation.z = loadSetting(settings, "translation_z", 0.0f);
        glm::vec3 eulers;
        eulers.x = loadSetting(settings, "rotation_x", 0.0f);
        eulers.y = loadSetting(settings, "rotation_y", 0.0f);
        eulers.z = loadSetting(settings, "rotation_z", 0.0f);
        attachment.rotation = glm::quat(eulers);
        attachment.scale = loadSetting(settings, "scale", 1.0f);
        attachmentData.append(attachment);
    }
    settings->endArray();
    setAttachmentData(attachmentData);
    
    setDisplayName(settings->value("displayName").toString());

    settings->endGroup();
}

int MyAvatar::parseDataAtOffset(const QByteArray& packet, int offset) {
    qDebug() << "Error: ignoring update packet for MyAvatar"
        << " packetLength = " << packet.size() 
        << "  offset = " << offset;
    // this packet is just bad, so we pretend that we unpacked it ALL
    return packet.size() - offset;
}

void MyAvatar::sendKillAvatar() {
    QByteArray killPacket = byteArrayWithPopulatedHeader(PacketTypeKillAvatar);
    NodeList::getInstance()->broadcastToNodes(killPacket, NodeSet() << NodeType::AvatarMixer);
}

void MyAvatar::updateLookAtTargetAvatar() {
    //
    //  Look at the avatar whose eyes are closest to the ray in direction of my avatar's head
    //
    _lookAtTargetAvatar.clear();
    _targetAvatarPosition = glm::vec3(0.0f);
    const float MIN_LOOKAT_ANGLE = PI / 4.0f;        //  Smallest angle between face and person where we will look at someone
    float smallestAngleTo = MIN_LOOKAT_ANGLE;
    foreach (const AvatarSharedPointer& avatarPointer, Application::getInstance()->getAvatarManager().getAvatarHash()) {
        Avatar* avatar = static_cast<Avatar*>(avatarPointer.data());
        avatar->setIsLookAtTarget(false);
        if (!avatar->isMyAvatar()) {
            float angleTo = glm::angle(getHead()->getFinalOrientation() * glm::vec3(0.0f, 0.0f, -1.0f),
                                       glm::normalize(avatar->getHead()->getEyePosition() - getHead()->getEyePosition()));
            if (angleTo < smallestAngleTo) {
                _lookAtTargetAvatar = avatarPointer;
                _targetAvatarPosition = avatarPointer->getPosition();
                smallestAngleTo = angleTo;
            }
        }
    }
    if (_lookAtTargetAvatar) {
        static_cast<Avatar*>(_lookAtTargetAvatar.data())->setIsLookAtTarget(true);
    }
}

void MyAvatar::clearLookAtTargetAvatar() {
    _lookAtTargetAvatar.clear();
}

glm::vec3 MyAvatar::getUprightHeadPosition() const {
    return _position + getWorldAlignedOrientation() * glm::vec3(0.0f, getPelvisToHeadLength(), 0.0f);
}

void MyAvatar::setJointData(int index, const glm::quat& rotation) {
    Avatar::setJointData(index, rotation);
    if (QThread::currentThread() == thread()) {
        _skeletonModel.setJointState(index, true, rotation);
    }
}

void MyAvatar::clearJointData(int index) {
    Avatar::clearJointData(index);
    if (QThread::currentThread() == thread()) {
        _skeletonModel.setJointState(index, false);
    }
}

void MyAvatar::setFaceModelURL(const QUrl& faceModelURL) {
    Avatar::setFaceModelURL(faceModelURL);
    _billboardValid = false;
}

void MyAvatar::setSkeletonModelURL(const QUrl& skeletonModelURL) {
    Avatar::setSkeletonModelURL(skeletonModelURL);
    _billboardValid = false;
}

void MyAvatar::renderBody(RenderMode renderMode, float glowLevel) {
    if (!(_skeletonModel.isRenderable() && getHead()->getFaceModel().isRenderable())) {
        return; // wait until both models are loaded
    }
    
    //  Render the body's voxels and head
    Model::RenderMode modelRenderMode = (renderMode == SHADOW_RENDER_MODE) ?
        Model::SHADOW_RENDER_MODE : Model::DEFAULT_RENDER_MODE;
    _skeletonModel.render(1.0f, modelRenderMode);
    renderAttachments(modelRenderMode);
    
    //  Render head so long as the camera isn't inside it
    if (shouldRenderHead(Application::getInstance()->getCamera()->getPosition(), renderMode)) {
        getHead()->render(1.0f, modelRenderMode);
    }
    getHand()->render(true);
}

const float RENDER_HEAD_CUTOFF_DISTANCE = 0.50f;

bool MyAvatar::shouldRenderHead(const glm::vec3& cameraPosition, RenderMode renderMode) const {
    const Head* head = getHead();
    return (renderMode != NORMAL_RENDER_MODE) || 
        (glm::length(cameraPosition - head->calculateAverageEyePosition()) > RENDER_HEAD_CUTOFF_DISTANCE * _scale);
}

void MyAvatar::updateOrientation(float deltaTime) {
    //  Gather rotation information from keyboard
    _bodyYawDelta -= _driveKeys[ROT_RIGHT] * YAW_SPEED * deltaTime;
    _bodyYawDelta += _driveKeys[ROT_LEFT] * YAW_SPEED * deltaTime;
    getHead()->setBasePitch(getHead()->getBasePitch() + (_driveKeys[ROT_UP] - _driveKeys[ROT_DOWN]) * PITCH_SPEED * deltaTime);

    // update body yaw by body yaw delta
    glm::quat orientation = getOrientation() * glm::quat(glm::radians(
                glm::vec3(_bodyPitchDelta, _bodyYawDelta, _bodyRollDelta) * deltaTime));

    // decay body rotation momentum
    const float BODY_SPIN_FRICTION = 7.5f;
    float bodySpinMomentum = 1.0f - BODY_SPIN_FRICTION * deltaTime;
    if (bodySpinMomentum < 0.0f) { bodySpinMomentum = 0.0f; }
    _bodyPitchDelta *= bodySpinMomentum;
    _bodyYawDelta *= bodySpinMomentum;
    _bodyRollDelta *= bodySpinMomentum;

    float MINIMUM_ROTATION_RATE = 2.0f;
    if (fabs(_bodyYawDelta) < MINIMUM_ROTATION_RATE) { _bodyYawDelta = 0.0f; }
    if (fabs(_bodyRollDelta) < MINIMUM_ROTATION_RATE) { _bodyRollDelta = 0.0f; }
    if (fabs(_bodyPitchDelta) < MINIMUM_ROTATION_RATE) { _bodyPitchDelta = 0.0f; }

    if (OculusManager::isConnected()) {
        // these angles will be in radians
        float yaw, pitch, roll; 
        OculusManager::getEulerAngles(yaw, pitch, roll);
        // ... so they need to be converted to degrees before we do math...

        // The neck is limited in how much it can yaw, so we check its relative
        // yaw from the body and yaw the body if necessary.
        yaw *= DEGREES_PER_RADIAN;
        float bodyToHeadYaw = yaw - _oculusYawOffset;
        const float MAX_NECK_YAW = 85.0f; // degrees
        if ((fabs(bodyToHeadYaw) > 2.0f * MAX_NECK_YAW) && (yaw * _oculusYawOffset < 0.0f)) {
            // We've wrapped around the range for yaw so adjust 
            // the measured yaw to be relative to _oculusYawOffset.
            if (yaw > 0.0f) {
                yaw -= 360.0f;
            } else {
                yaw += 360.0f;
            }
            bodyToHeadYaw = yaw - _oculusYawOffset;
        }

        float delta = fabs(bodyToHeadYaw) - MAX_NECK_YAW;
        if (delta > 0.0f) {
            yaw = MAX_NECK_YAW;
            if (bodyToHeadYaw < 0.0f) {
                delta *= -1.0f;
                bodyToHeadYaw = -MAX_NECK_YAW;
            } else {
                bodyToHeadYaw = MAX_NECK_YAW;
            }
            // constrain _oculusYawOffset to be within range [-180,180]
            _oculusYawOffset = fmod((_oculusYawOffset + delta) + 180.0f, 360.0f) - 180.0f;

            // We must adjust the body orientation using a delta rotation (rather than
            // doing yaw math) because the body's yaw ranges are not the same
            // as what the Oculus API provides.
            glm::vec3 UP_AXIS = glm::vec3(0.0f, 1.0f, 0.0f);
            glm::quat bodyCorrection = glm::angleAxis(glm::radians(delta), UP_AXIS);
            orientation = orientation * bodyCorrection;
        }
        Head* head = getHead();
        head->setBaseYaw(bodyToHeadYaw);

        head->setBasePitch(pitch * DEGREES_PER_RADIAN);
        head->setBaseRoll(roll * DEGREES_PER_RADIAN);
    }

    // update the euler angles
    setOrientation(orientation);
}

void MyAvatar::updateMotorFromKeyboard(float deltaTime, bool walking) {
    // Increase motor velocity until its length is equal to _maxMotorSpeed.
    if (!(_motionBehaviors & AVATAR_MOTION_MOTOR_KEYBOARD_ENABLED)) {
        // nothing to do
        return;
    }

    glm::vec3 localVelocity = _velocity;
    if (_motionBehaviors & AVATAR_MOTION_MOTOR_USE_LOCAL_FRAME) {
        glm::quat orientation = getHead()->getCameraOrientation();
        localVelocity = glm::inverse(orientation) * _velocity;
    }

    // Compute keyboard input
    glm::vec3 front = (_driveKeys[FWD] - _driveKeys[BACK]) * IDENTITY_FRONT;
    glm::vec3 right = (_driveKeys[RIGHT] - _driveKeys[LEFT]) * IDENTITY_RIGHT;
    glm::vec3 up = (_driveKeys[UP] - _driveKeys[DOWN]) * IDENTITY_UP;

    glm::vec3 direction = front + right + up;
    float directionLength = glm::length(direction);

    // Compute motor magnitude
    if (directionLength > EPSILON) {
        direction /= directionLength;
        // the finalMotorSpeed depends on whether we are walking or not
        const float MIN_KEYBOARD_CONTROL_SPEED = 2.0f;
        const float MAX_WALKING_SPEED = 3.0f * MIN_KEYBOARD_CONTROL_SPEED;
        float finalMaxMotorSpeed = walking ? MAX_WALKING_SPEED : _maxMotorSpeed;

        float motorLength = glm::length(_motorVelocity);
        if (motorLength < MIN_KEYBOARD_CONTROL_SPEED) {
            // an active keyboard motor should never be slower than this
            _motorVelocity = MIN_KEYBOARD_CONTROL_SPEED * direction;
        } else {
            float MOTOR_LENGTH_TIMESCALE = 1.5f;
            float tau = glm::clamp(deltaTime / MOTOR_LENGTH_TIMESCALE, 0.0f, 1.0f);
            float INCREASE_FACTOR = 2.0f;
            //_motorVelocity *= 1.0f + tau * INCREASE_FACTOR;
            motorLength *= 1.0f + tau * INCREASE_FACTOR;
            if (motorLength > finalMaxMotorSpeed) {
                motorLength = finalMaxMotorSpeed;
            }
            _motorVelocity = motorLength * direction;
        }
        _isPushing = true;
    } else {
        // motor opposes motion (wants to be at rest)
        _motorVelocity = - localVelocity;
    }
}

float MyAvatar::computeMotorTimescale() {
    // The timescale of the motor is the approximate time it takes for the motor to 
    // accomplish its intended velocity.  A short timescale makes the motor strong, 
    // and a long timescale makes it weak.  The value of timescale to use depends 
    // on what the motor is doing:
    //
    // (1) braking --> short timescale (aggressive motor assertion)
    // (2) pushing --> medium timescale (mild motor assertion)
    // (3) inactive --> long timescale (gentle friction for low speeds)
    //
    // TODO: recover extra braking behavior when flying close to nearest avatar

    float MIN_MOTOR_TIMESCALE = 0.125f;
    float MAX_MOTOR_TIMESCALE = 0.5f;
    float MIN_BRAKE_SPEED = 0.4f;

    float timescale = MAX_MOTOR_TIMESCALE;
    float speed = glm::length(_velocity);
    bool areThrusting = (glm::length2(_thrust) > EPSILON);

    if (_wasPushing && !(_isPushing || areThrusting) && speed > MIN_BRAKE_SPEED) {
        // we don't change _wasPushing for this case --> 
        // keeps the brakes on until we go below MIN_BRAKE_SPEED
        timescale = MIN_MOTOR_TIMESCALE;
    } else {
        if (_isPushing) {
            timescale = _motorTimescale;
        } 
        _wasPushing = _isPushing || areThrusting;
    }
    _isPushing = false;
    return timescale;
}

void MyAvatar::applyMotor(float deltaTime) {
    if (!( _motionBehaviors & AVATAR_MOTION_MOTOR_ENABLED)) {
        // nothing to do --> early exit
        return;
    }
    glm::vec3 targetVelocity = _motorVelocity;
    if (_motionBehaviors & AVATAR_MOTION_MOTOR_USE_LOCAL_FRAME) {
        // rotate _motorVelocity into world frame
        glm::quat rotation = getHead()->getCameraOrientation();
        targetVelocity = rotation * _motorVelocity;
    }

    glm::vec3 targetDirection(0.0f);
    if (glm::length2(targetVelocity) > EPSILON) {
        targetDirection = glm::normalize(targetVelocity);
    }
    glm::vec3 deltaVelocity = targetVelocity - _velocity;

    if (_motionBehaviors & AVATAR_MOTION_MOTOR_COLLISION_SURFACE_ONLY && glm::length2(_gravity) > EPSILON) {
        // For now we subtract the component parallel to gravity but what we need to do is: 
        // TODO: subtract the component perp to the local surface normal (motor only pushes in surface plane).
        glm::vec3 gravityDirection = glm::normalize(_gravity);
        glm::vec3 parallelDelta = glm::dot(deltaVelocity, gravityDirection) * gravityDirection;
        if (glm::dot(targetVelocity, _velocity) > 0.0f) {
            // remove parallel part from deltaVelocity
            deltaVelocity -= parallelDelta;
        }
    }

    // simple critical damping
    float timescale = computeMotorTimescale();
    float tau = glm::clamp(deltaTime / timescale, 0.0f, 1.0f);
    _velocity += tau * deltaVelocity;
}

void MyAvatar::applyThrust(float deltaTime) {
    _velocity += _thrust * deltaTime;
    float speed = glm::length(_velocity);
    // cap the speed that thrust can achieve
    if (speed > MAX_AVATAR_SPEED) {
        _velocity *= MAX_AVATAR_SPEED / speed;
    }
    // zero thrust so we don't pile up thrust from other sources
    _thrust = glm::vec3(0.0f);
}

/* Keep this code for the short term as reference in case we need to further tune the new model 
 * to achieve legacy movement response.
void MyAvatar::updateThrust(float deltaTime) {
    //
    //  Gather thrust information from keyboard and sensors to apply to avatar motion
    //
    glm::quat orientation = getHead()->getCameraOrientation();
    glm::vec3 front = orientation * IDENTITY_FRONT;
    glm::vec3 right = orientation * IDENTITY_RIGHT;
    glm::vec3 up = orientation * IDENTITY_UP;

    const float THRUST_MAG_UP = 800.0f;
    const float THRUST_MAG_DOWN = 300.0f;
    const float THRUST_MAG_FWD = 500.0f;
    const float THRUST_MAG_BACK = 300.0f;
    const float THRUST_MAG_LATERAL = 250.0f;
    const float THRUST_JUMP = 120.0f;

    //  Add Thrusts from keyboard
    _thrust += _driveKeys[FWD] * _scale * THRUST_MAG_FWD * _thrustMultiplier * deltaTime * front;
    _thrust -= _driveKeys[BACK] * _scale * THRUST_MAG_BACK *  _thrustMultiplier * deltaTime * front;
    _thrust += _driveKeys[RIGHT] * _scale * THRUST_MAG_LATERAL * _thrustMultiplier * deltaTime * right;
    _thrust -= _driveKeys[LEFT] * _scale * THRUST_MAG_LATERAL * _thrustMultiplier * deltaTime * right;
    _thrust += _driveKeys[UP] * _scale * THRUST_MAG_UP * _thrustMultiplier * deltaTime * up;
    _thrust -= _driveKeys[DOWN] * _scale * THRUST_MAG_DOWN * _thrustMultiplier * deltaTime * up;

    // attenuate thrust when in penetration
    if (glm::dot(_thrust, _lastBodyPenetration) > EPSILON) {
        const float MAX_BODY_PENETRATION_DEPTH = 0.6f * _skeletonModel.getBoundingShapeRadius();
        float penetrationFactor = glm::min(1.0f, glm::length(_lastBodyPenetration) / MAX_BODY_PENETRATION_DEPTH);
        glm::vec3 penetrationDirection = glm::normalize(_lastBodyPenetration);
        // attenuate parallel component
        glm::vec3 parallelThrust = glm::dot(_thrust, penetrationDirection) * penetrationDirection;
        // attenuate perpendicular component (friction)
        glm::vec3 perpendicularThrust = _thrust - parallelThrust;
        // recombine to get the final thrust
        _thrust = (1.0f - penetrationFactor) * parallelThrust + (1.0f - penetrationFactor * penetrationFactor) * perpendicularThrust;

        // attenuate the growth of _thrustMultiplier when in penetration
        // otherwise the avatar will eventually be able to tunnel through the obstacle
        _thrustMultiplier *= (1.0f - penetrationFactor * penetrationFactor);
    } else if (_thrustMultiplier < 1.0f) {
        // rapid healing of attenuated thrustMultiplier after penetration event
        _thrustMultiplier = 1.0f;
    }
    _lastBodyPenetration = glm::vec3(0.0f);

    //  If thrust keys are being held down, slowly increase thrust to allow reaching great speeds
    if (_driveKeys[FWD] || _driveKeys[BACK] || _driveKeys[RIGHT] || _driveKeys[LEFT] || _driveKeys[UP] || _driveKeys[DOWN]) {
        const float THRUST_INCREASE_RATE = 1.05f;
        const float MAX_THRUST_MULTIPLIER = 75.0f;
        _thrustMultiplier *= 1.0f + deltaTime * THRUST_INCREASE_RATE;
        if (_thrustMultiplier > MAX_THRUST_MULTIPLIER) {
            _thrustMultiplier = MAX_THRUST_MULTIPLIER;
        }
    } else {
        _thrustMultiplier = 1.0f;
    }

    //  Add one time jumping force if requested
    if (_shouldJump) {
        if (glm::length(_gravity) > EPSILON) {
            _thrust += _scale * THRUST_JUMP * up;
        }
        _shouldJump = false;
    }

    //  Update speed brake status
    const float MIN_SPEED_BRAKE_VELOCITY = _scale * 0.4f;
    if ((glm::length(_thrust) == 0.0f) && _isThrustOn && (glm::length(_velocity) > MIN_SPEED_BRAKE_VELOCITY)) {
        _speedBrakes = true;
    }
    _isThrustOn = (glm::length(_thrust) > EPSILON);

    if (_isThrustOn || (_speedBrakes && (glm::length(_velocity) < MIN_SPEED_BRAKE_VELOCITY))) {
        _speedBrakes = false;
    }
    _velocity += _thrust * deltaTime;

    // Zero thrust out now that we've added it to velocity in this frame
    _thrust = glm::vec3(0.0f);

    // apply linear damping
    const float MAX_STATIC_FRICTION_SPEED = 0.5f;
    const float STATIC_FRICTION_STRENGTH = _scale * 20.0f;
    applyStaticFriction(deltaTime, _velocity, MAX_STATIC_FRICTION_SPEED, STATIC_FRICTION_STRENGTH);

    const float LINEAR_DAMPING_STRENGTH = 0.5f;
    const float SPEED_BRAKE_POWER = _scale * 10.0f;
    const float SQUARED_DAMPING_STRENGTH = 0.007f;

    const float SLOW_NEAR_RADIUS = 5.0f;
    float linearDamping = LINEAR_DAMPING_STRENGTH;
    const float NEAR_AVATAR_DAMPING_FACTOR = 50.0f;
    if (_distanceToNearestAvatar < _scale * SLOW_NEAR_RADIUS) {
        linearDamping *= 1.0f + NEAR_AVATAR_DAMPING_FACTOR *
                            ((SLOW_NEAR_RADIUS - _distanceToNearestAvatar) / SLOW_NEAR_RADIUS);
    }
    if (_speedBrakes) {
        applyDamping(deltaTime, _velocity,  linearDamping * SPEED_BRAKE_POWER, SQUARED_DAMPING_STRENGTH * SPEED_BRAKE_POWER);
    } else {
        applyDamping(deltaTime, _velocity, linearDamping, SQUARED_DAMPING_STRENGTH);
    }
}
*/

void MyAvatar::updateHandMovementAndTouching(float deltaTime) {
    glm::quat orientation = getOrientation();

    // reset hand and arm positions according to hand movement
    glm::vec3 up = orientation * IDENTITY_UP;

    bool pointing = false;
    if (glm::length(_mouseRayDirection) > EPSILON && !Application::getInstance()->isMouseHidden()) {
        // confine to the approximate shoulder plane
        glm::vec3 pointDirection = _mouseRayDirection;
        if (glm::dot(_mouseRayDirection, up) > 0.0f) {
            glm::vec3 projectedVector = glm::cross(up, glm::cross(_mouseRayDirection, up));
            if (glm::length(projectedVector) > EPSILON) {
                pointDirection = glm::normalize(projectedVector);
            }
        }
        glm::vec3 shoulderPosition;
        if (_skeletonModel.getRightShoulderPosition(shoulderPosition)) {
            glm::vec3 farVector = _mouseRayOrigin + pointDirection * (float)TREE_SCALE - shoulderPosition;
            const float ARM_RETRACTION = 0.75f;
            float retractedLength = _skeletonModel.getRightArmLength() * ARM_RETRACTION;
            setHandPosition(shoulderPosition + glm::normalize(farVector) * retractedLength);
            pointing = true;
        }
    }

    if (_mousePressed) {
        _handState = HAND_STATE_GRASPING;
    } else if (pointing) {
        _handState = HAND_STATE_POINTING;
    } else {
        _handState = HAND_STATE_NULL;
    }
}

void MyAvatar::updateCollisionWithEnvironment(float deltaTime, float radius) {
    glm::vec3 up = getBodyUpDirection();
    const float ENVIRONMENT_SURFACE_ELASTICITY = 0.0f;
    const float ENVIRONMENT_SURFACE_DAMPING = 0.01f;
    const float ENVIRONMENT_COLLISION_FREQUENCY = 0.05f;
    glm::vec3 penetration;
    float pelvisFloatingHeight = getPelvisFloatingHeight();
    if (Application::getInstance()->getEnvironment()->findCapsulePenetration(
            _position - up * (pelvisFloatingHeight - radius),
            _position + up * (getSkeletonHeight() - pelvisFloatingHeight + radius), radius, penetration)) {
        updateCollisionSound(penetration, deltaTime, ENVIRONMENT_COLLISION_FREQUENCY);
        applyHardCollision(penetration, ENVIRONMENT_SURFACE_ELASTICITY, ENVIRONMENT_SURFACE_DAMPING);
    }
}

static CollisionList myCollisions(64);

void MyAvatar::updateCollisionWithVoxels(float deltaTime, float radius) {
    const float MIN_STUCK_SPEED = 100.0f;
    float speed = glm::length(_velocity);
    if (speed > MIN_STUCK_SPEED) {
        // don't even bother to try to collide against voxles when moving very fast
        return;
    }
    myCollisions.clear();
    const CapsuleShape& boundingShape = _skeletonModel.getBoundingShape();
    if (Application::getInstance()->getVoxelTree()->findShapeCollisions(&boundingShape, myCollisions)) {
        const float VOXEL_ELASTICITY = 0.0f;
        const float VOXEL_DAMPING = 0.0f;
        float capsuleRadius = boundingShape.getRadius();

        glm::vec3 totalPenetration(0.0f);
        bool isStuck = false;
        for (int i = 0; i < myCollisions.size(); ++i) {
            CollisionInfo* collision = myCollisions[i];
            float depth = glm::length(collision->_penetration);
            if (depth > capsuleRadius) {
                isStuck = true;
                if (_wasStuck) {
                    glm::vec3 cubeCenter = collision->_vecData;
                    float cubeSide = collision->_floatData;
                    float distance = glm::dot(boundingShape.getPosition() - cubeCenter, _worldUpDirection);
                    if (distance < 0.0f) {
                        distance = fabsf(distance) + 0.5f * cubeSide;
                    }
                    distance += capsuleRadius + boundingShape.getHalfHeight();
                    totalPenetration = addPenetrations(totalPenetration, - distance * _worldUpDirection);
                    continue;
                }
            }
            totalPenetration = addPenetrations(totalPenetration, collision->_penetration);
        }
        applyHardCollision(totalPenetration, VOXEL_ELASTICITY, VOXEL_DAMPING);
        _wasStuck = isStuck;

        const float VOXEL_COLLISION_FREQUENCY = 0.5f;
        updateCollisionSound(myCollisions[0]->_penetration, deltaTime, VOXEL_COLLISION_FREQUENCY);
    } else {
        _wasStuck = false;
    }
}

void MyAvatar::applyHardCollision(const glm::vec3& penetration, float elasticity, float damping) {
    //
    //  Update the avatar in response to a hard collision.  Position will be reset exactly
    //  to outside the colliding surface.  Velocity will be modified according to elasticity.
    //
    //  if elasticity = 0.0, collision is 100% inelastic.
    //  if elasticity = 1.0, collision is elastic.
    //
    _position -= penetration;
    static float HALTING_VELOCITY = 0.2f;
    // cancel out the velocity component in the direction of penetration
    float penetrationLength = glm::length(penetration);
    if (penetrationLength > EPSILON) {
        glm::vec3 direction = penetration / penetrationLength;
        _velocity -= glm::dot(_velocity, direction) * direction * (1.0f + elasticity);
        _velocity *= glm::clamp(1.0f - damping, 0.0f, 1.0f);
        if ((glm::length(_velocity) < HALTING_VELOCITY) && (glm::length(_thrust) == 0.0f)) {
            // If moving really slowly after a collision, and not applying forces, stop altogether
            _velocity *= 0.0f;
        }
    }
}

void MyAvatar::updateCollisionSound(const glm::vec3 &penetration, float deltaTime, float frequency) {
    //  consider whether to have the collision make a sound
    const float AUDIBLE_COLLISION_THRESHOLD = 0.02f;
    const float COLLISION_LOUDNESS = 1.0f;
    const float DURATION_SCALING = 0.004f;
    const float NOISE_SCALING = 0.1f;
    glm::vec3 velocity = _velocity;
    glm::vec3 gravity = getGravity();

    if (glm::length(gravity) > EPSILON) {
        //  If gravity is on, remove the effect of gravity on velocity for this
        //  frame, so that we are not constantly colliding with the surface
        velocity -= _scale * glm::length(gravity) * GRAVITY_EARTH * deltaTime * glm::normalize(gravity);
    }
    float velocityTowardCollision = glm::dot(velocity, glm::normalize(penetration));
    float velocityTangentToCollision = glm::length(velocity) - velocityTowardCollision;

    if (velocityTowardCollision > AUDIBLE_COLLISION_THRESHOLD) {
        //  Volume is proportional to collision velocity
        //  Base frequency is modified upward by the angle of the collision
        //  Noise is a function of the angle of collision
        //  Duration of the sound is a function of both base frequency and velocity of impact
        Application::getInstance()->getAudio()->startCollisionSound(
            std::min(COLLISION_LOUDNESS * velocityTowardCollision, 1.0f),
            frequency * (1.0f + velocityTangentToCollision / velocityTowardCollision),
            std::min(velocityTangentToCollision / velocityTowardCollision * NOISE_SCALING, 1.0f),
            1.0f - DURATION_SCALING * powf(frequency, 0.5f) / velocityTowardCollision, false);
    }
}

bool findAvatarAvatarPenetration(const glm::vec3 positionA, float radiusA, float heightA,
        const glm::vec3 positionB, float radiusB, float heightB, glm::vec3& penetration) {
    glm::vec3 positionBA = positionB - positionA;
    float xzDistance = sqrt(positionBA.x * positionBA.x + positionBA.z * positionBA.z);
    if (xzDistance < (radiusA + radiusB)) {
        float yDistance = fabs(positionBA.y);
        float halfHeights = 0.5 * (heightA + heightB);
        if (yDistance < halfHeights) {
            // cylinders collide
            if (xzDistance > 0.0f) {
                positionBA.y = 0.0f;
                // note, penetration should point from A into B
                penetration = positionBA * ((radiusA + radiusB - xzDistance) / xzDistance);
                return true;
            } else {
                // exactly coaxial -- we'll return false for this case
                return false;
            }
        } else if (yDistance < halfHeights + radiusA + radiusB) {
            // caps collide
            if (positionBA.y < 0.0f) {
                // A is above B
                positionBA.y += halfHeights;
                float BA = glm::length(positionBA);
                penetration = positionBA * (radiusA + radiusB - BA) / BA;
                return true;
            } else {
                // A is below B
                positionBA.y -= halfHeights;
                float BA = glm::length(positionBA);
                penetration = positionBA * (radiusA + radiusB - BA) / BA;
                return true;
            }
        }
    }
    return false;
}

const float BODY_COLLISION_RESOLUTION_TIMESCALE = 0.5f; // seconds

void MyAvatar::updateCollisionWithAvatars(float deltaTime) {
    //  Reset detector for nearest avatar
    _distanceToNearestAvatar = std::numeric_limits<float>::max();
    const AvatarHash& avatars = Application::getInstance()->getAvatarManager().getAvatarHash();
    if (avatars.size() <= 1) {
        // no need to compute a bunch of stuff if we have one or fewer avatars
        return;
    }
    float myBoundingRadius = getBoundingRadius();

    const float BODY_COLLISION_RESOLUTION_FACTOR = glm::max(1.0f, deltaTime / BODY_COLLISION_RESOLUTION_TIMESCALE);

    foreach (const AvatarSharedPointer& avatarPointer, avatars) {
        Avatar* avatar = static_cast<Avatar*>(avatarPointer.data());
        if (static_cast<Avatar*>(this) == avatar) {
            // don't collide with ourselves
            continue;
        }
        avatar->updateShapePositions();
        float distance = glm::length(_position - avatar->getPosition());        
        if (_distanceToNearestAvatar > distance) {
            _distanceToNearestAvatar = distance;
        }
        float theirBoundingRadius = avatar->getBoundingRadius();
        if (distance < myBoundingRadius + theirBoundingRadius) {
            // collide our body against theirs
            QVector<const Shape*> myShapes;
            _skeletonModel.getBodyShapes(myShapes);
            QVector<const Shape*> theirShapes;
            avatar->getSkeletonModel().getBodyShapes(theirShapes);

            CollisionInfo collision;
            if (ShapeCollider::collideShapesCoarse(myShapes, theirShapes, collision)) {
                float penetrationDepth = glm::length(collision._penetration);
                if (penetrationDepth > myBoundingRadius) {
                    qDebug() << "WARNING: ignoring avatar-avatar penetration depth " << penetrationDepth;
                }
                else if (penetrationDepth > EPSILON) {
                    setPosition(getPosition() - BODY_COLLISION_RESOLUTION_FACTOR * collision._penetration);
                    _lastBodyPenetration += collision._penetration;
                    emit collisionWithAvatar(getSessionUUID(), avatar->getSessionUUID(), collision);
                }
            }

            // collide our hands against them
            // TODO: make this work when we can figure out when the other avatar won't yeild
            // (for example, we're colliding against their chest or leg)
            //getHand()->collideAgainstAvatar(avatar, true);

            // collide their hands against us
            avatar->getHand()->collideAgainstAvatar(this, false);
        }
    }
    // TODO: uncomment this when we handle collisions that won't affect other avatar
    //getHand()->resolvePenetrations();
}

class SortedAvatar {
public:
    Avatar* avatar;
    float distance;
    glm::vec3 accumulatedCenter;
};

bool operator<(const SortedAvatar& s1, const SortedAvatar& s2) {
    return s1.distance < s2.distance;
}

void MyAvatar::updateChatCircle(float deltaTime) {
    if (!(_isChatCirclingEnabled = Menu::getInstance()->isOptionChecked(MenuOption::ChatCircling))) {
        return;
    }

    // find all circle-enabled members and sort by distance
    QVector<SortedAvatar> sortedAvatars;
    
    foreach (const AvatarSharedPointer& avatarPointer, Application::getInstance()->getAvatarManager().getAvatarHash()) {
        Avatar* avatar = static_cast<Avatar*>(avatarPointer.data());
        if ( ! avatar->isChatCirclingEnabled() ||
                avatar == static_cast<Avatar*>(this)) {
            continue;
        }
    
        SortedAvatar sortedAvatar;
        sortedAvatar.avatar = avatar;
        sortedAvatar.distance = glm::distance(_position, sortedAvatar.avatar->getPosition());
        sortedAvatars.append(sortedAvatar);
    }
    
    qSort(sortedAvatars.begin(), sortedAvatars.end());

    // compute the accumulated centers
    glm::vec3 center = _position;
    for (int i = 0; i < sortedAvatars.size(); i++) {
        SortedAvatar& sortedAvatar = sortedAvatars[i];
        sortedAvatar.accumulatedCenter = (center += sortedAvatar.avatar->getPosition()) / (i + 2.0f);
    }

    // remove members whose accumulated circles are too far away to influence us
    const float CIRCUMFERENCE_PER_MEMBER = 0.5f;
    const float CIRCLE_INFLUENCE_SCALE = 2.0f;
    const float MIN_RADIUS = 0.3f;
    for (int i = sortedAvatars.size() - 1; i >= 0; i--) {
        float radius = qMax(MIN_RADIUS, (CIRCUMFERENCE_PER_MEMBER * (i + 2)) / TWO_PI);
        if (glm::distance(_position, sortedAvatars[i].accumulatedCenter) > radius * CIRCLE_INFLUENCE_SCALE) {
            sortedAvatars.remove(i);
        } else {
            break;
        }
    }
    if (sortedAvatars.isEmpty()) {
        return;
    }
    center = sortedAvatars.last().accumulatedCenter;
    float radius = qMax(MIN_RADIUS, (CIRCUMFERENCE_PER_MEMBER * (sortedAvatars.size() + 1)) / TWO_PI);

    // compute the average up vector
    glm::vec3 up = getWorldAlignedOrientation() * IDENTITY_UP;
    foreach (const SortedAvatar& sortedAvatar, sortedAvatars) {
        up += sortedAvatar.avatar->getWorldAlignedOrientation() * IDENTITY_UP;
    }
    up = glm::normalize(up);

    // find reasonable corresponding right/front vectors
    glm::vec3 front = glm::cross(up, IDENTITY_RIGHT);
    if (glm::length(front) < EPSILON) {
        front = glm::cross(up, IDENTITY_FRONT);
    }
    front = glm::normalize(front);
    glm::vec3 right = glm::cross(front, up);

    // find our angle and the angular distances to our closest neighbors
    glm::vec3 delta = _position - center;
    glm::vec3 projected = glm::vec3(glm::dot(right, delta), glm::dot(front, delta), 0.0f);
    float myAngle = glm::length(projected) > EPSILON ? atan2f(projected.y, projected.x) : 0.0f;
    float leftDistance = TWO_PI;
    float rightDistance = TWO_PI;
    foreach (const SortedAvatar& sortedAvatar, sortedAvatars) {
        delta = sortedAvatar.avatar->getPosition() - center;
        projected = glm::vec3(glm::dot(right, delta), glm::dot(front, delta), 0.0f);
        float angle = glm::length(projected) > EPSILON ? atan2f(projected.y, projected.x) : 0.0f;
        if (angle < myAngle) {
            leftDistance = min(myAngle - angle, leftDistance);
            rightDistance = min(TWO_PI - (myAngle - angle), rightDistance);

        } else {
            leftDistance = min(TWO_PI - (angle - myAngle), leftDistance);
            rightDistance = min(angle - myAngle, rightDistance);
        }
    }

    // if we're on top of a neighbor, we need to randomize so that they don't both go in the same direction
    if (rightDistance == 0.0f && randomBoolean()) {
        swap(leftDistance, rightDistance);
    }

    // split the difference between our neighbors
    float targetAngle = myAngle + (rightDistance - leftDistance) / 4.0f;
    glm::vec3 targetPosition = center + (front * sinf(targetAngle) + right * cosf(targetAngle)) * radius;

    // approach the target position
    const float APPROACH_RATE = 0.05f;
    _position = glm::mix(_position, targetPosition, APPROACH_RATE);
}

void MyAvatar::maybeUpdateBillboard() {
    if (_billboardValid || !(_skeletonModel.isLoadedWithTextures() && getHead()->getFaceModel().isLoadedWithTextures())) {
        return;
    }
    QImage image = Application::getInstance()->renderAvatarBillboard();
    _billboard.clear();
    QBuffer buffer(&_billboard);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    _billboardValid = true;
    
    sendBillboardPacket();
}

void MyAvatar::goHome() {
    qDebug("Going Home!");
    setPosition(START_LOCATION);
}

void MyAvatar::increaseSize() {
    if ((1.0f + SCALING_RATIO) * _targetScale < MAX_AVATAR_SCALE) {
        _targetScale *= (1.0f + SCALING_RATIO);
        qDebug("Changed scale to %f", _targetScale);
    }
}

void MyAvatar::decreaseSize() {
    if (MIN_AVATAR_SCALE < (1.0f - SCALING_RATIO) * _targetScale) {
        _targetScale *= (1.0f - SCALING_RATIO);
        qDebug("Changed scale to %f", _targetScale);
    }
}

void MyAvatar::resetSize() {
    _targetScale = 1.0f;
    qDebug("Reseted scale to %f", _targetScale);
}

static QByteArray createByteArray(const glm::vec3& vector) {
    return QByteArray::number(vector.x) + ',' + QByteArray::number(vector.y) + ',' + QByteArray::number(vector.z);
}

void MyAvatar::updateLocationInDataServer() {
    // TODO: don't re-send this when it hasn't change or doesn't change by some threshold
    // This will required storing the last sent values and clearing them when the AccountManager rootURL changes
    
    AccountManager& accountManager = AccountManager::getInstance();
    
    if (accountManager.isLoggedIn()) {
        QString positionString(createByteArray(_position));
        QString orientationString(createByteArray(glm::degrees(safeEulerAngles(getOrientation()))));
        
        // construct the json to put the user's location
        QString locationPutJson = QString() + "{\"address\":{\"position\":\""
            + positionString + "\", \"orientation\":\"" + orientationString + "\"}}";
        
        accountManager.authenticatedRequest("/api/v1/users/address", QNetworkAccessManager::PutOperation,
                                            JSONCallbackParameters(), locationPutJson.toUtf8());
    }
}

void MyAvatar::goToLocationFromResponse(const QJsonObject& jsonObject) {
    
    if (jsonObject["status"].toString() == "success") {
        
        // send a node kill request, indicating to other clients that they should play the "disappeared" effect
        sendKillAvatar();
        
        QJsonObject locationObject = jsonObject["data"].toObject()["address"].toObject();
        QString positionString = locationObject["position"].toString();
        QString orientationString = locationObject["orientation"].toString();
        QString domainHostnameString = locationObject["domain"].toString();
        
        qDebug() << "Changing domain to" << domainHostnameString <<
            ", position to" << positionString <<
            ", and orientation to" << orientationString;
        
        QStringList coordinateItems = positionString.split(',');
        QStringList orientationItems = orientationString.split(',');
        
        NodeList::getInstance()->getDomainHandler().setHostname(domainHostnameString);
        
        // orient the user to face the target
        glm::quat newOrientation = glm::quat(glm::radians(glm::vec3(orientationItems[0].toFloat(),
                                                                    orientationItems[1].toFloat(),
                                                                    orientationItems[2].toFloat())))
            * glm::angleAxis(PI, glm::vec3(0.0f, 1.0f, 0.0f));
        setOrientation(newOrientation);
        
        // move the user a couple units away
        const float DISTANCE_TO_USER = 2.0f;
        glm::vec3 newPosition = glm::vec3(coordinateItems[0].toFloat(), coordinateItems[1].toFloat(),
                                          coordinateItems[2].toFloat()) - newOrientation * IDENTITY_FRONT * DISTANCE_TO_USER;
        setPosition(newPosition);
        emit transformChanged();
    } else {
        QMessageBox::warning(Application::getInstance()->getWindow(), "", "That user or location could not be found.");
    }
}

void MyAvatar::updateMotionBehaviorsFromMenu() {
    if (Menu::getInstance()->isOptionChecked(MenuOption::ObeyEnvironmentalGravity)) {
        _motionBehaviors |= AVATAR_MOTION_OBEY_ENVIRONMENTAL_GRAVITY;
        // Environmental and Local gravities are incompatible.  Environmental setting trumps local.
        _motionBehaviors &= ~AVATAR_MOTION_OBEY_LOCAL_GRAVITY;
    } else {
        _motionBehaviors &= ~AVATAR_MOTION_OBEY_ENVIRONMENTAL_GRAVITY;
    }
    if (! (_motionBehaviors & (AVATAR_MOTION_OBEY_ENVIRONMENTAL_GRAVITY | AVATAR_MOTION_OBEY_LOCAL_GRAVITY))) {
        setGravity(glm::vec3(0.0f));
    }
}

void MyAvatar::setCollisionGroups(quint32 collisionGroups) {
    Avatar::setCollisionGroups(collisionGroups & VALID_COLLISION_GROUPS);
    Menu* menu = Menu::getInstance();
    menu->setIsOptionChecked(MenuOption::CollideWithEnvironment, (bool)(_collisionGroups & COLLISION_GROUP_ENVIRONMENT));
    menu->setIsOptionChecked(MenuOption::CollideWithAvatars, (bool)(_collisionGroups & COLLISION_GROUP_AVATARS));
    menu->setIsOptionChecked(MenuOption::CollideWithVoxels, (bool)(_collisionGroups & COLLISION_GROUP_VOXELS));
    menu->setIsOptionChecked(MenuOption::CollideWithParticles, (bool)(_collisionGroups & COLLISION_GROUP_PARTICLES));
}

void MyAvatar::setMotionBehaviorsByScript(quint32 flags) {
    // start with the defaults
    _motionBehaviors = AVATAR_MOTION_DEFAULTS;

    // add the set scriptable bits
    _motionBehaviors += flags & AVATAR_MOTION_SCRIPTABLE_BITS;

    // reconcile incompatible settings from menu (if any)
    Menu* menu = Menu::getInstance();
    menu->setIsOptionChecked(MenuOption::ObeyEnvironmentalGravity, (bool)(_motionBehaviors & AVATAR_MOTION_OBEY_ENVIRONMENTAL_GRAVITY));
    // Environmental and Local gravities are incompatible.  Environmental setting trumps local.
    if (_motionBehaviors & AVATAR_MOTION_OBEY_ENVIRONMENTAL_GRAVITY) {
        _motionBehaviors &= ~AVATAR_MOTION_OBEY_LOCAL_GRAVITY;
        setGravity(Application::getInstance()->getEnvironment()->getGravity(getPosition()));
    } else if (! (_motionBehaviors & (AVATAR_MOTION_OBEY_ENVIRONMENTAL_GRAVITY | AVATAR_MOTION_OBEY_LOCAL_GRAVITY))) {
        setGravity(glm::vec3(0.0f));
    }
}

void MyAvatar::applyCollision(const glm::vec3& contactPoint, const glm::vec3& penetration) {
    glm::vec3 leverAxis = contactPoint - getPosition();
    float leverLength = glm::length(leverAxis);
    if (leverLength > EPSILON) {
        // compute lean perturbation angles
        glm::quat bodyRotation = getOrientation();
        glm::vec3 xAxis = bodyRotation * glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 zAxis = bodyRotation * glm::vec3(0.0f, 0.0f, 1.0f);

        leverAxis = leverAxis / leverLength;
        glm::vec3 effectivePenetration = penetration - glm::dot(penetration, leverAxis) * leverAxis;
        // use the small-angle approximation for sine
        float sideways = - glm::dot(effectivePenetration, xAxis) / leverLength;
        float forward = glm::dot(effectivePenetration, zAxis) / leverLength;
        getHead()->addLeanDeltas(sideways, forward);
    }
}
