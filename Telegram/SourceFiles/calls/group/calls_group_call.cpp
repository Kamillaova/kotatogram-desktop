/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/group/calls_group_call.h"

#include "calls/group/calls_group_common.h"
#include "main/main_session.h"
#include "api/api_send_progress.h"
#include "api/api_updates.h"
#include "apiwrap.h"
#include "lang/lang_keys.h"
#include "lang/lang_hardcoded.h"
#include "boxes/peers/edit_participants_box.h" // SubscribeToMigration.
#include "ui/toasts/common_toasts.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_group_call.h"
#include "data/data_session.h"
#include "base/global_shortcuts.h"
#include "base/openssl_help.h"
#include "webrtc/webrtc_video_track.h"
#include "webrtc/webrtc_media_devices.h"
#include "webrtc/webrtc_create_adm.h"

#include <tgcalls/group/GroupInstanceCustomImpl.h>
#include <tgcalls/VideoCaptureInterface.h>
#include <tgcalls/StaticThreads.h>
#include <xxhash.h>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>

namespace Calls {
namespace {

constexpr auto kMaxInvitePerSlice = 10;
constexpr auto kCheckLastSpokeInterval = crl::time(1000);
constexpr auto kCheckJoinedTimeout = 4 * crl::time(1000);
constexpr auto kUpdateSendActionEach = crl::time(500);
constexpr auto kPlayConnectingEach = crl::time(1056) + 2 * crl::time(1000);

[[nodiscard]] std::unique_ptr<Webrtc::MediaDevices> CreateMediaDevices() {
	const auto &settings = Core::App().settings();
	return Webrtc::CreateMediaDevices(
		settings.callInputDeviceId(),
		settings.callOutputDeviceId(),
		settings.callVideoInputDeviceId());
}

[[nodiscard]] const Data::GroupCall::Participant *LookupParticipant(
		not_null<PeerData*> peer,
		uint64 id,
		not_null<PeerData*> participantPeer) {
	const auto call = peer->groupCall();
	if (!id || !call || call->id() != id) {
		return nullptr;
	}
	const auto &participants = call->participants();
	const auto i = ranges::find(
		participants,
		participantPeer,
		&Data::GroupCall::Participant::peer);
	return (i != end(participants)) ? &*i : nullptr;
}

[[nodiscard]] double TimestampFromMsgId(mtpMsgId msgId) {
	return msgId / double(1ULL << 32);
}

[[nodiscard]] std::string ReadJsonString(
		const QJsonObject &object,
		const char *key) {
	return object.value(key).toString().toStdString();
};

} // namespace

struct VideoParams {
	tgcalls::GroupParticipantDescription description;
	base::flat_set<uint32> videoSsrcs;
	uint32 hash = 0;
};

class GroupCall::LoadPartTask final : public tgcalls::BroadcastPartTask {
public:
	LoadPartTask(
		base::weak_ptr<GroupCall> call,
		int64 time,
		int64 period,
		Fn<void(tgcalls::BroadcastPart&&)> done);

	[[nodiscard]] int64 time() const {
		return _time;
	}
	[[nodiscard]] int32 scale() const {
		return _scale;
	}

	void done(tgcalls::BroadcastPart &&part);
	void cancel() override;

private:
	const base::weak_ptr<GroupCall> _call;
	const int64 _time = 0;
	const int32 _scale = 0;
	Fn<void(tgcalls::BroadcastPart &&)> _done;
	QMutex _mutex;

};

struct GroupCall::LargeTrack {
	LargeTrack() : track(Webrtc::VideoState::Active) {
	}

	Webrtc::VideoTrack track;
	std::shared_ptr<Webrtc::SinkInterface> sink;
};

[[nodiscard]] bool IsGroupCallAdmin(
		not_null<PeerData*> peer,
		not_null<PeerData*> participantPeer) {
	const auto user = participantPeer->asUser();
	if (!user) {
		return false;
	}
	if (const auto chat = peer->asChat()) {
		return chat->admins.contains(user)
			|| (chat->creator == peerToUser(user->id));
	} else if (const auto group = peer->asChannel()) {
		if (const auto mgInfo = group->mgInfo.get()) {
			if (mgInfo->creator == user) {
				return true;
			}
			const auto i = mgInfo->lastAdmins.find(user);
			if (i == mgInfo->lastAdmins.end()) {
				return false;
			}
			const auto &rights = i->second.rights;
			return rights.c_chatAdminRights().is_manage_call();
		}
	}
	return false;
}

std::shared_ptr<VideoParams> ParseVideoParams(
		const QByteArray &json,
		const std::shared_ptr<VideoParams> &existing) {
	using namespace tgcalls;

	if (json.isEmpty()) {
		return nullptr;
	}
	const auto hash = XXH32(json.data(), json.size(), uint32(0));
	if (existing && existing->hash == hash) {
		return existing;
	}
	// We don't reuse existing pointer, that way we can compare pointers
	// to see if anything was changed in video params.
	const auto data = /*existing
		? existing
		: */std::make_shared<VideoParams>();
	data->hash = hash;

	auto error = QJsonParseError{ 0, QJsonParseError::NoError };
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError) {
		LOG(("API Error: "
			"Failed to parse group call video params, error: %1."
			).arg(error.errorString()));
		return data;
	} else if (!document.isObject()) {
		LOG(("API Error: "
			"Not an object received in group call video params."));
		return data;
	}

	const auto object = document.object();
	data->description.endpointId = ReadJsonString(object, "endpoint");

	const auto ssrcGroups = object.value("ssrc-groups").toArray();
	data->description.videoSourceGroups.reserve(ssrcGroups.size());
	for (const auto &value : ssrcGroups) {
		const auto inner = value.toObject();
		auto sources = std::vector<uint32_t>();
		{
			const auto list = inner.value("sources").toArray();
			sources.reserve(list.size());
			for (const auto &source : list) {
				const auto ssrc = uint32_t(source.toDouble());
				sources.push_back(ssrc);
				data->videoSsrcs.emplace(ssrc);
			}
		}
		data->description.videoSourceGroups.push_back({
			.ssrcs = std::move(sources),
			.semantics = ReadJsonString(inner, "semantics"),
		});
	}

	// videoPayloadTypes and videoExtensionMap will be in _commonVideoFields.
	return data;
}

const base::flat_set<uint32> &VideoSourcesFromParams(
		const std::shared_ptr<VideoParams> &params) {
	static const auto kEmpty = base::flat_set<uint32>();
	return (params && !params->videoSsrcs.empty())
		? params->videoSsrcs
		: kEmpty;
}

GroupCall::LoadPartTask::LoadPartTask(
	base::weak_ptr<GroupCall> call,
	int64 time,
	int64 period,
	Fn<void(tgcalls::BroadcastPart &&)> done)
: _call(std::move(call))
, _time(time ? time : (base::unixtime::now() * int64(1000)))
, _scale([&] {
	switch (period) {
	case 1000: return 0;
	case 500: return 1;
	case 250: return 2;
	case 125: return 3;
	}
	Unexpected("Period in LoadPartTask.");
}())
, _done(std::move(done)) {
}

void GroupCall::LoadPartTask::done(tgcalls::BroadcastPart &&part) {
	QMutexLocker lock(&_mutex);
	if (_done) {
		base::take(_done)(std::move(part));
	}
}

void GroupCall::LoadPartTask::cancel() {
	QMutexLocker lock(&_mutex);
	if (!_done) {
		return;
	}
	_done = nullptr;
	lock.unlock();

	if (_call) {
		const auto that = this;
		crl::on_main(_call, [weak = _call, that] {
			if (const auto strong = weak.get()) {
				strong->broadcastPartCancel(that);
			}
		});
	}
}

GroupCall::GroupCall(
	not_null<Delegate*> delegate,
	Group::JoinInfo info,
	const MTPInputGroupCall &inputCall)
: _delegate(delegate)
, _peer(info.peer)
, _history(_peer->owner().history(_peer))
, _api(&_peer->session().mtp())
, _joinAs(info.joinAs)
, _possibleJoinAs(std::move(info.possibleJoinAs))
, _joinHash(info.joinHash)
, _id(inputCall.c_inputGroupCall().vid().v)
, _scheduleDate(info.scheduleDate)
, _videoOutgoing(
	std::make_unique<Webrtc::VideoTrack>(Webrtc::VideoState::Inactive))
, _lastSpokeCheckTimer([=] { checkLastSpoke(); })
, _checkJoinedTimer([=] { checkJoined(); })
, _pushToTalkCancelTimer([=] { pushToTalkCancel(); })
, _connectingSoundTimer([=] { playConnectingSoundOnce(); })
, _mediaDevices(CreateMediaDevices()) {
	_muted.value(
	) | rpl::combine_previous(
	) | rpl::start_with_next([=](MuteState previous, MuteState state) {
		if (_instance) {
			updateInstanceMuteState();
		}
		if (_mySsrc
			&& (!_initialMuteStateSent || state == MuteState::Active)) {
			_initialMuteStateSent = true;
			maybeSendMutedUpdate(previous);
		}
	}, _lifetime);

	_instanceState.value(
	) | rpl::filter([=] {
		return _hadJoinedState;
	}) | rpl::start_with_next([=](InstanceState state) {
		if (state == InstanceState::Disconnected) {
			playConnectingSound();
		} else {
			stopConnectingSound();
		}
	}, _lifetime);

	checkGlobalShortcutAvailability();

	if (const auto real = lookupReal()) {
		subscribeToReal(real);
		if (!_peer->canManageGroupCall() && real->joinMuted()) {
			_muted = MuteState::ForceMuted;
		}
	} else {
		_peer->session().changes().peerFlagsValue(
			_peer,
			Data::PeerUpdate::Flag::GroupCall
		) | rpl::map([=] {
			return lookupReal();
		}) | rpl::filter([](Data::GroupCall *real) {
			return real != nullptr;
		}) | rpl::map([](Data::GroupCall *real) {
			return not_null{ real };
		}) | rpl::take(
			1
		) | rpl::start_with_next([=](not_null<Data::GroupCall*> real) {
			subscribeToReal(real);
			_realChanges.fire_copy(real);
		}, _lifetime);
	}

	setupMediaDevices();

	if (_id) {
		join(inputCall);
	} else {
		start(info.scheduleDate);
	}
	if (_scheduleDate) {
		saveDefaultJoinAs(_joinAs);
	}
}

GroupCall::~GroupCall() {
	destroyController();
	const auto wasScreenSharing = isScreenSharing();
	const auto weak = wasScreenSharing
		? std::weak_ptr<tgcalls::VideoCaptureInterface>(_videoCapture)
		: std::weak_ptr<tgcalls::VideoCaptureInterface>();
	_videoCapture = nullptr;
	if (const auto strong = weak.lock()) {
		strong->switchToDevice(_videoDeviceId.toStdString());
	}
}

bool GroupCall::isScreenSharing() const {
	return (_videoDeviceId != _videoInputId);
}

void GroupCall::toggleVideo(bool active) {
	if (!active) {
		if (_videoOutgoing->state() != Webrtc::VideoState::Inactive) {
			_videoOutgoing->setState(Webrtc::VideoState::Inactive);
			sendSelfUpdate(SendUpdateType::VideoMuted);
		}
		return;
	}
	const auto changing = isScreenSharing();
	_videoDeviceId = _videoInputId;
	if (_videoOutgoing->state() != Webrtc::VideoState::Active) {
		_videoOutgoing->setState(Webrtc::VideoState::Active);
		sendSelfUpdate(SendUpdateType::VideoMuted);
	}
	if (!_videoCapture) {
		return;
	}
	if (changing) {
		_videoCapture->switchToDevice(_videoDeviceId.toStdString());
	}
}

void GroupCall::switchToScreenSharing(const QString &uniqueId) {
	if (_videoDeviceId == uniqueId) {
		return;
	}
	_videoDeviceId = uniqueId;
	if (_videoOutgoing->state() != Webrtc::VideoState::Active) {
		_videoOutgoing->setState(Webrtc::VideoState::Active);
		sendSelfUpdate(SendUpdateType::VideoMuted);
	}
	_videoCapture->switchToDevice(_videoDeviceId.toStdString());
}

void GroupCall::setScheduledDate(TimeId date) {
	const auto was = _scheduleDate;
	_scheduleDate = date;
	if (was && !date) {
		join(inputCall());
	}
}

void GroupCall::subscribeToReal(not_null<Data::GroupCall*> real) {
	real->scheduleDateValue(
	) | rpl::start_with_next([=](TimeId date) {
		setScheduledDate(date);
	}, _lifetime);

	using Update = Data::GroupCall::ParticipantUpdate;
	real->participantUpdated(
	) | rpl::start_with_next([=](const Update &data) {
		auto newLarge = _videoStreamLarge.current();
		auto updateAsNotStreams = uint32(0);
		const auto guard = gsl::finally([&] {
			if (!newLarge) {
				newLarge = chooseLargeVideoSsrc();
			}
			if (_videoStreamLarge.current() != newLarge) {
				_videoStreamLarge = newLarge;
			}
			if (updateAsNotStreams) {
				_streamsVideoUpdated.fire({ updateAsNotStreams, false });
			}
		});

		const auto wasVideoMutedSsrc = (data.was && data.was->videoMuted)
			? data.was->ssrc
			: 0;
		const auto nowVideoMutedSsrc = (data.now && data.now->videoMuted)
			? data.now->ssrc
			: 0;
		if (wasVideoMutedSsrc != nowVideoMutedSsrc) {
			if (wasVideoMutedSsrc
				&& _videoMuted.remove(wasVideoMutedSsrc)
				&& _videoStreamSsrcs.contains(wasVideoMutedSsrc)
				&& data.now
				&& data.now->ssrc == wasVideoMutedSsrc) {
				_streamsVideoUpdated.fire({ wasVideoMutedSsrc, true });
			}
			if (nowVideoMutedSsrc
				&& _videoMuted.emplace(nowVideoMutedSsrc).second
				&& _videoStreamSsrcs.contains(nowVideoMutedSsrc)) {
				updateAsNotStreams = nowVideoMutedSsrc;
				if (newLarge == nowVideoMutedSsrc) {
					newLarge = 0;
				}
			}
		}
		const auto nowSpeaking = data.now && data.now->speaking;
		const auto nowSounding = data.now && data.now->sounding;
		const auto wasSpeaking = data.was && data.was->speaking;
		const auto wasSounding = data.was && data.was->sounding;
		if (nowSpeaking == wasSpeaking && nowSounding == wasSounding) {
			return;
		} else if (_videoStreamPinned) {
			return;
		}
		const auto &participants = real->participants();
		if ((wasSpeaking || wasSounding)
			&& (data.was->ssrc == newLarge)) {
			auto bestWithVideoSsrc = uint32(0);
			for (const auto &participant : participants) {
				if (!participant.sounding
					|| !streamsVideo(participant.ssrc)) {
					continue;
				}
				if (participant.speaking) {
					bestWithVideoSsrc = participant.ssrc;
					break;
				} else if (!bestWithVideoSsrc) {
					bestWithVideoSsrc = participant.ssrc;
				}
			}
			if (bestWithVideoSsrc) {
				newLarge = bestWithVideoSsrc;
			}
		} else if ((nowSpeaking || nowSounding)
			&& (data.now->ssrc != newLarge)
			&& streamsVideo(data.now->ssrc)) {
			const auto i = ranges::find(
				participants,
				newLarge,
				&Data::GroupCallParticipant::ssrc);
			const auto speaking = (i != end(participants)) && i->speaking;
			const auto sounding = (i != end(participants)) && i->sounding;
			if ((nowSpeaking && !speaking) || (nowSounding && !sounding)) {
				newLarge = data.now->ssrc;
			}
		}
	}, _lifetime);
}

void GroupCall::checkGlobalShortcutAvailability() {
	auto &settings = Core::App().settings();
	if (!settings.groupCallPushToTalk()) {
		return;
	} else if (!base::GlobalShortcutsAllowed()) {
		settings.setGroupCallPushToTalk(false);
		Core::App().saveSettingsDelayed();
	}
}

void GroupCall::setState(State state) {
	if (_state.current() == State::Failed) {
		return;
	} else if (_state.current() == State::FailedHangingUp
		&& state != State::Failed) {
		return;
	}
	if (_state.current() == state) {
		return;
	}
	_state = state;

	if (state == State::Joined) {
		stopConnectingSound();
		if (const auto call = _peer->groupCall(); call && call->id() == _id) {
			call->setInCall();
		}
	}

	if (false
		|| state == State::Ended
		|| state == State::Failed) {
		// Destroy controller before destroying Call Panel,
		// so that the panel hide animation is smooth.
		destroyController();
	}
	switch (state) {
	case State::HangingUp:
	case State::FailedHangingUp:
		_delegate->groupCallPlaySound(Delegate::GroupCallSound::Ended);
		break;
	case State::Ended:
		_delegate->groupCallFinished(this);
		break;
	case State::Failed:
		_delegate->groupCallFailed(this);
		break;
	case State::Connecting:
		if (!_checkJoinedTimer.isActive()) {
			_checkJoinedTimer.callOnce(kCheckJoinedTimeout);
		}
		break;
	}
}

void GroupCall::playConnectingSound() {
	if (_connectingSoundTimer.isActive()) {
		return;
	}
	playConnectingSoundOnce();
	_connectingSoundTimer.callEach(kPlayConnectingEach);
}

void GroupCall::stopConnectingSound() {
	_connectingSoundTimer.cancel();
}

void GroupCall::playConnectingSoundOnce() {
	_delegate->groupCallPlaySound(Delegate::GroupCallSound::Connecting);
}

bool GroupCall::showChooseJoinAs() const {
	return (_possibleJoinAs.size() > 1)
		|| (_possibleJoinAs.size() == 1
			&& !_possibleJoinAs.front()->isSelf());
}

bool GroupCall::scheduleStartSubscribed() const {
	if (const auto real = lookupReal()) {
		return real->scheduleStartSubscribed();
	}
	return false;
}

Data::GroupCall *GroupCall::lookupReal() const {
	const auto real = _peer->groupCall();
	return (real && real->id() == _id) ? real : nullptr;
}

rpl::producer<not_null<Data::GroupCall*>> GroupCall::real() const {
	if (const auto real = lookupReal()) {
		return rpl::single(not_null{ real });
	}
	return _realChanges.events();
}

void GroupCall::start(TimeId scheduleDate) {
	using Flag = MTPphone_CreateGroupCall::Flag;
	_createRequestId = _api.request(MTPphone_CreateGroupCall(
		MTP_flags(scheduleDate ? Flag::f_schedule_date : Flag(0)),
		_peer->input,
		MTP_int(openssl::RandomValue<int32>()),
		MTPstring(), // title
		MTP_int(scheduleDate)
	)).done([=](const MTPUpdates &result) {
		_acceptFields = true;
		_peer->session().api().applyUpdates(result);
		_acceptFields = false;
	}).fail([=](const MTP::Error &error) {
		LOG(("Call Error: Could not create, error: %1"
			).arg(error.type()));
		hangup();
		if (error.type() == u"GROUPCALL_ANONYMOUS_FORBIDDEN"_q) {
			Ui::ShowMultilineToast({
				.text = { tr::lng_group_call_no_anonymous(tr::now) },
			});
		}
	}).send();
}

void GroupCall::join(const MTPInputGroupCall &inputCall) {
	inputCall.match([&](const MTPDinputGroupCall &data) {
		_id = data.vid().v;
		_accessHash = data.vaccess_hash().v;
	});
	setState(_scheduleDate ? State::Waiting : State::Joining);

	if (_scheduleDate) {
		return;
	}
	rejoin();

	using Update = Data::GroupCall::ParticipantUpdate;
	_peer->groupCall()->participantUpdated(
	) | rpl::filter([=](const Update &update) {
		return (_instance != nullptr);
	}) | rpl::start_with_next([=](const Update &update) {
		if (!update.now) {
			_instance->removeSsrcs({ update.was->ssrc });
		} else {
			const auto &now = *update.now;
			const auto &was = update.was;
			const auto volumeChanged = was
				? (was->volume != now.volume
					|| was->mutedByMe != now.mutedByMe)
				: (now.volume != Group::kDefaultVolume || now.mutedByMe);
			if (now.videoParams
				&& now.ssrc
				&& (!was
					|| was->videoParams != now.videoParams
					|| was->ssrc != now.ssrc)
				&& (now.peer != _joinAs)
				&& (_instanceMode != InstanceMode::None)) {
				prepareParticipantForAdding(now);
				addPreparedParticipantsDelayed();
			}

			if (volumeChanged) {
				_instance->setVolume(
					now.ssrc,
					(now.mutedByMe
						? 0.
						: (now.volume
							/ float64(Group::kDefaultVolume))));
			}
		}
	}, _lifetime);

	addParticipantsToInstance();

	_peer->session().updates().addActiveChat(
		_peerStream.events_starting_with_copy(_peer));
	SubscribeToMigration(_peer, _lifetime, [=](not_null<ChannelData*> group) {
		_peer = group;
		_peerStream.fire_copy(group);
	});
}

void GroupCall::rejoin() {
	rejoin(_joinAs);
}

void GroupCall::rejoinWithHash(const QString &hash) {
	if (!hash.isEmpty()
		&& (muted() == MuteState::ForceMuted
			|| muted() == MuteState::RaisedHand)) {
		_joinHash = hash;
		rejoin();
	}
}

void GroupCall::setJoinAs(not_null<PeerData*> as) {
	_joinAs = as;
	if (const auto chat = _peer->asChat()) {
		chat->setGroupCallDefaultJoinAs(_joinAs->id);
	} else if (const auto channel = _peer->asChannel()) {
		channel->setGroupCallDefaultJoinAs(_joinAs->id);
	}
}

void GroupCall::saveDefaultJoinAs(not_null<PeerData*> as) {
	setJoinAs(as);
	_api.request(MTPphone_SaveDefaultGroupCallJoinAs(
		_peer->input,
		_joinAs->input
	)).send();
}

void GroupCall::rejoin(not_null<PeerData*> as) {
	if (state() != State::Joining
		&& state() != State::Joined
		&& state() != State::Connecting) {
		return;
	}

	_mySsrc = 0;
	_initialMuteStateSent = false;
	setState(State::Joining);
	ensureControllerCreated();
	setInstanceMode(InstanceMode::None);
	applyMeInCallLocally();
	LOG(("Call Info: Requesting join payload."));

	setJoinAs(as);

	const auto weak = base::make_weak(this);
	_instance->emitJoinPayload([=](tgcalls::GroupJoinPayload payload) {
		crl::on_main(weak, [=, payload = std::move(payload)]{
			auto fingerprints = QJsonArray();
			for (const auto &print : payload.fingerprints) {
				auto object = QJsonObject();
				object.insert("hash", QString::fromStdString(print.hash));
				object.insert("setup", QString::fromStdString(print.setup));
				object.insert(
					"fingerprint",
					QString::fromStdString(print.fingerprint));
				fingerprints.push_back(object);
			}

			auto extensionMap = QJsonArray();
			for (const auto &extension : payload.videoExtensionMap) {
				auto object = QJsonObject();
				object.insert("id", int64(extension.first));
				object.insert(
					"uri",
					QString::fromStdString(extension.second));
				extensionMap.push_back(object);
			}

			auto payloadTypes = QJsonArray();
			for (const auto &type : payload.videoPayloadTypes) {
				auto object = QJsonObject();
				object.insert("id", int64(type.id));
				object.insert("name", QString::fromStdString(type.name));
				object.insert("clockrate", int64(type.clockrate));
				if (!type.parameters.empty()) {
					auto parameters = QJsonObject();
					for (const auto &parameter : type.parameters) {
						parameters.insert(
							QString::fromStdString(parameter.first),
							QString::fromStdString(parameter.second));
					}
					object.insert("parameters", parameters);
				}
				if (type.name != "rtx") {
					object.insert("channels", int64(type.channels));
					auto fbs = QJsonArray();
					for (const auto &element : type.feedbackTypes) {
						auto inner = QJsonObject();
						inner.insert(
							"type",
							QString::fromStdString(element.type));
						if (!element.subtype.empty()) {
							inner.insert(
								"subtype",
								QString::fromStdString(element.subtype));
						}
						fbs.push_back(inner);
					}
					object.insert("rtcp-fbs", fbs);
				}
				payloadTypes.push_back(object);
			}

			auto sourceGroups = QJsonArray();
			for (const auto &group : payload.videoSourceGroups) {
				auto object = QJsonObject();
				object.insert(
					"semantics",
					QString::fromStdString(group.semantics));
				auto list = QJsonArray();
				for (const auto source : group.ssrcs) {
					list.push_back(int64(source));
				}
				object.insert("sources", list);
				sourceGroups.push_back(object);
			}

			auto root = QJsonObject();
			const auto ssrc = payload.ssrc;
			root.insert("ufrag", QString::fromStdString(payload.ufrag));
			root.insert("pwd", QString::fromStdString(payload.pwd));
			root.insert("fingerprints", fingerprints);
			root.insert("ssrc", double(payload.ssrc));
			if (!extensionMap.isEmpty()) {
				root.insert("rtp-hdrexts", extensionMap);
			}
			if (!payloadTypes.isEmpty()) {
				root.insert("payload-types", payloadTypes);
			}
			if (!sourceGroups.isEmpty()) {
				root.insert("ssrc-groups", sourceGroups);
			}

			LOG(("Call Info: Join payload received, joining with ssrc: %1."
				).arg(ssrc));

			const auto json = QJsonDocument(root).toJson(
				QJsonDocument::Compact);
			const auto wasMuteState = muted();
			using Flag = MTPphone_JoinGroupCall::Flag;
			_api.request(MTPphone_JoinGroupCall(
				MTP_flags((wasMuteState != MuteState::Active
					? Flag::f_muted
					: Flag(0)) | (_joinHash.isEmpty()
						? Flag(0)
						: Flag::f_invite_hash)),
				inputCall(),
				_joinAs->input,
				MTP_string(_joinHash),
				MTP_dataJSON(MTP_bytes(json))
			)).done([=](const MTPUpdates &updates) {
				_mySsrc = ssrc;
				_mySsrcs.emplace(ssrc);
				setState((_instanceState.current()
					== InstanceState::Disconnected)
					? State::Connecting
					: State::Joined);
				applyMeInCallLocally();
				maybeSendMutedUpdate(wasMuteState);
				_peer->session().api().applyUpdates(updates);
				applyQueuedSelfUpdates();
				checkFirstTimeJoined();
				sendSelfUpdate(SendUpdateType::VideoMuted);
			}).fail([=](const MTP::Error &error) {
				const auto type = error.type();
				LOG(("Call Error: Could not join, error: %1").arg(type));

				if (type == u"GROUPCALL_SSRC_DUPLICATE_MUCH") {
					rejoin();
					return;
				}

				hangup();
				Ui::ShowMultilineToast({
					.text = { type == u"GROUPCALL_ANONYMOUS_FORBIDDEN"_q
						? tr::lng_group_call_no_anonymous(tr::now)
						: type == u"GROUPCALL_PARTICIPANTS_TOO_MUCH"_q
						? tr::lng_group_call_too_many(tr::now)
						: type == u"GROUPCALL_FORBIDDEN"_q
						? tr::lng_group_not_accessible(tr::now)
						: Lang::Hard::ServerError() },
				});
			}).send();
		});
	});
}

[[nodiscard]] uint64 FindLocalRaisedHandRating(
		const std::vector<Data::GroupCallParticipant> &list) {
	const auto i = ranges::max_element(
		list,
		ranges::less(),
		&Data::GroupCallParticipant::raisedHandRating);
	return (i == end(list)) ? 1 : (i->raisedHandRating + 1);
}

void GroupCall::applyMeInCallLocally() {
	const auto call = _peer->groupCall();
	if (!call || call->id() != _id) {
		return;
	}
	using Flag = MTPDgroupCallParticipant::Flag;
	const auto &participants = call->participants();
	const auto i = ranges::find(
		participants,
		_joinAs,
		&Data::GroupCall::Participant::peer);
	const auto date = (i != end(participants))
		? i->date
		: base::unixtime::now();
	const auto lastActive = (i != end(participants))
		? i->lastActive
		: TimeId(0);
	const auto volume = (i != end(participants))
		? i->volume
		: Group::kDefaultVolume;
	const auto canSelfUnmute = (muted() != MuteState::ForceMuted)
		&& (muted() != MuteState::RaisedHand);
	const auto videoMuted = (_videoOutgoing->state()
		!= Webrtc::VideoState::Active);
	const auto raisedHandRating = (muted() != MuteState::RaisedHand)
		? uint64(0)
		: (i != end(participants))
		? i->raisedHandRating
		: FindLocalRaisedHandRating(participants);
	const auto flags = (canSelfUnmute ? Flag::f_can_self_unmute : Flag(0))
		| (lastActive ? Flag::f_active_date : Flag(0))
		| (_mySsrc ? Flag(0) : Flag::f_left)
		| Flag::f_self
		| Flag::f_volume // Without flag the volume is reset to 100%.
		| Flag::f_volume_by_admin // Self volume can only be set by admin.
		| ((muted() != MuteState::Active) ? Flag::f_muted : Flag(0))
		//| (videoMuted ? Flag(0) : Flag::f_video)
		| (raisedHandRating > 0 ? Flag::f_raise_hand_rating : Flag(0));
	call->applyLocalUpdate(
		MTP_updateGroupCallParticipants(
			inputCall(),
			MTP_vector<MTPGroupCallParticipant>(
				1,
				MTP_groupCallParticipant(
					MTP_flags(flags),
					peerToMTP(_joinAs->id),
					MTP_int(date),
					MTP_int(lastActive),
					MTP_int(_mySsrc),
					MTP_int(volume),
					MTPstring(), // Don't update about text in local updates.
					MTP_long(raisedHandRating),
					MTPDataJSON(), // video
					MTPDataJSON())), // presentation
			MTP_int(0)).c_updateGroupCallParticipants());
}

void GroupCall::applyParticipantLocally(
		not_null<PeerData*> participantPeer,
		bool mute,
		std::optional<int> volume) {
	const auto participant = LookupParticipant(_peer, _id, participantPeer);
	if (!participant || !participant->ssrc) {
		return;
	}
	const auto canManageCall = _peer->canManageGroupCall();
	const auto isMuted = participant->muted || (mute && canManageCall);
	const auto canSelfUnmute = !canManageCall
		? participant->canSelfUnmute
		: (!mute || IsGroupCallAdmin(_peer, participantPeer));
	const auto isMutedByYou = mute && !canManageCall;
	const auto mutedCount = 0/*participant->mutedCount*/;
	using Flag = MTPDgroupCallParticipant::Flag;
	const auto flags = (canSelfUnmute ? Flag::f_can_self_unmute : Flag(0))
		| Flag::f_volume // Without flag the volume is reset to 100%.
		| ((participant->applyVolumeFromMin && !volume)
			? Flag::f_volume_by_admin
			: Flag(0))
		| (participant->lastActive ? Flag::f_active_date : Flag(0))
		| (isMuted ? Flag::f_muted : Flag(0))
		| (isMutedByYou ? Flag::f_muted_by_you : Flag(0))
		| (participantPeer == _joinAs ? Flag::f_self : Flag(0))
		//| (participant->videoMuted ? Flag(0) : Flag::f_video)
		| (participant->raisedHandRating
			? Flag::f_raise_hand_rating
			: Flag(0));
	_peer->groupCall()->applyLocalUpdate(
		MTP_updateGroupCallParticipants(
			inputCall(),
			MTP_vector<MTPGroupCallParticipant>(
				1,
				MTP_groupCallParticipant(
					MTP_flags(flags),
					peerToMTP(participantPeer->id),
					MTP_int(participant->date),
					MTP_int(participant->lastActive),
					MTP_int(participant->ssrc),
					MTP_int(volume.value_or(participant->volume)),
					MTPstring(), // Don't update about text in local updates.
					MTP_long(participant->raisedHandRating),
					MTPDataJSON(), // video
					MTPDataJSON())), // presentation
			MTP_int(0)).c_updateGroupCallParticipants());
}

void GroupCall::hangup() {
	finish(FinishType::Ended);
}

void GroupCall::discard() {
	if (!_id) {
		_api.request(_createRequestId).cancel();
		hangup();
		return;
	}
	_api.request(MTPphone_DiscardGroupCall(
		inputCall()
	)).done([=](const MTPUpdates &result) {
		// Here 'this' could be destroyed by updates, so we set Ended after
		// updates being handled, but in a guarded way.
		crl::on_main(this, [=] { hangup(); });
		_peer->session().api().applyUpdates(result);
	}).fail([=](const MTP::Error &error) {
		hangup();
	}).send();
}

void GroupCall::rejoinAs(Group::JoinInfo info) {
	_possibleJoinAs = std::move(info.possibleJoinAs);
	if (info.joinAs == _joinAs) {
		return;
	}
	const auto event = Group::RejoinEvent{
		.wasJoinAs = _joinAs,
		.nowJoinAs = info.joinAs,
	};
	if (_scheduleDate) {
		saveDefaultJoinAs(info.joinAs);
	} else {
		setState(State::Joining);
		rejoin(info.joinAs);
	}
	_rejoinEvents.fire_copy(event);
}

void GroupCall::finish(FinishType type) {
	Expects(type != FinishType::None);

	const auto finalState = (type == FinishType::Ended)
		? State::Ended
		: State::Failed;
	const auto hangupState = (type == FinishType::Ended)
		? State::HangingUp
		: State::FailedHangingUp;
	const auto state = _state.current();
	if (state == State::HangingUp
		|| state == State::FailedHangingUp
		|| state == State::Ended
		|| state == State::Failed) {
		return;
	}
	if (!_mySsrc) {
		setState(finalState);
		return;
	}

	setState(hangupState);

	// We want to leave request still being sent and processed even if
	// the call is already destroyed.
	const auto session = &_peer->session();
	const auto weak = base::make_weak(this);
	session->api().request(MTPphone_LeaveGroupCall(
		inputCall(),
		MTP_int(_mySsrc)
	)).done([=](const MTPUpdates &result) {
		// Here 'this' could be destroyed by updates, so we set Ended after
		// updates being handled, but in a guarded way.
		crl::on_main(weak, [=] { setState(finalState); });
		session->api().applyUpdates(result);
	}).fail(crl::guard(weak, [=](const MTP::Error &error) {
		setState(finalState);
	})).send();
}

void GroupCall::startScheduledNow() {
	if (!lookupReal()) {
		return;
	}
	_api.request(MTPphone_StartScheduledGroupCall(
		inputCall()
	)).done([=](const MTPUpdates &result) {
		_peer->session().api().applyUpdates(result);
	}).send();
}

void GroupCall::toggleScheduleStartSubscribed(bool subscribed) {
	if (!lookupReal()) {
		return;
	}
	_api.request(MTPphone_ToggleGroupCallStartSubscription(
		inputCall(),
		MTP_bool(subscribed)
	)).done([=](const MTPUpdates &result) {
		_peer->session().api().applyUpdates(result);
	}).send();
}

void GroupCall::addVideoOutput(
		uint32 ssrc,
		not_null<Webrtc::VideoTrack*> track) {
	if (_instance) {
		_instance->addIncomingVideoOutput(ssrc, track->sink());
	}
}

not_null<Webrtc::VideoTrack*> GroupCall::outgoingVideoTrack() const {
	return _videoOutgoing.get();
}

void GroupCall::setMuted(MuteState mute) {
	const auto set = [=] {
		const auto wasMuted = (muted() == MuteState::Muted)
			|| (muted() == MuteState::PushToTalk);
		const auto wasRaiseHand = (muted() == MuteState::RaisedHand);
		_muted = mute;
		const auto nowMuted = (muted() == MuteState::Muted)
			|| (muted() == MuteState::PushToTalk);
		const auto nowRaiseHand = (muted() == MuteState::RaisedHand);
		if (wasMuted != nowMuted || wasRaiseHand != nowRaiseHand) {
			applyMeInCallLocally();
		}
	};
	if (mute == MuteState::Active || mute == MuteState::PushToTalk) {
		_delegate->groupCallRequestPermissionsOrFail(crl::guard(this, set));
	} else {
		set();
	}
}

void GroupCall::setMutedAndUpdate(MuteState mute) {
	const auto was = muted();

	// Active state is sent from _muted changes,
	// because it may be set delayed, after permissions request, not now.
	const auto send = _initialMuteStateSent && (mute != MuteState::Active);
	setMuted(mute);
	if (send) {
		maybeSendMutedUpdate(was);
	}
}

void GroupCall::handlePossibleCreateOrJoinResponse(
		const MTPDupdateGroupCall &data) {
	data.vcall().match([&](const MTPDgroupCall &data) {
		handlePossibleCreateOrJoinResponse(data);
	}, [&](const MTPDgroupCallDiscarded &data) {
		handlePossibleDiscarded(data);
	});
}

void GroupCall::handlePossibleCreateOrJoinResponse(
		const MTPDgroupCall &data) {
	setScheduledDate(data.vschedule_date().value_or_empty());
	if (_acceptFields) {
		if (!_instance && !_id) {
			const auto input = MTP_inputGroupCall(
				data.vid(),
				data.vaccess_hash());
			const auto scheduleDate = data.vschedule_date().value_or_empty();
			if (const auto chat = _peer->asChat()) {
				chat->setGroupCall(input, scheduleDate);
			} else if (const auto group = _peer->asChannel()) {
				group->setGroupCall(input, scheduleDate);
			} else {
				Unexpected("Peer type in GroupCall::join.");
			}
			join(input);
		}
		return;
	} else if (_id != data.vid().v || !_instance) {
		return;
	}
	if (const auto streamDcId = data.vstream_dc_id()) {
		_broadcastDcId = MTP::BareDcId(streamDcId->v);
	}
}

void GroupCall::handlePossibleCreateOrJoinResponse(
		const MTPDupdateGroupCallConnection &data) {
	if (data.is_presentation()) {
		// #TODO calls
		return;
	}
	data.vparams().match([&](const MTPDdataJSON &data) {
		auto error = QJsonParseError{ 0, QJsonParseError::NoError };
		const auto document = QJsonDocument::fromJson(
			data.vdata().v,
			&error);
		if (error.error != QJsonParseError::NoError) {
			LOG(("API Error: "
				"Failed to parse group call params, error: %1."
				).arg(error.errorString()));
			return;
		} else if (!document.isObject()) {
			LOG(("API Error: "
				"Not an object received in group call params."));
			return;
		}

		const auto guard = gsl::finally([&] {
			addParticipantsToInstance();
		});

		if (document.object().value("stream").toBool()) {
			if (!_broadcastDcId) {
				LOG(("Api Error: Empty stream_dc_id in groupCall."));
				_broadcastDcId = _peer->session().mtp().mainDcId();
			}
			setInstanceMode(InstanceMode::Stream);
			return;
		}

		const auto readString = [](
			const QJsonObject &object,
			const char *key) {
			return object.value(key).toString().toStdString();
		};
		const auto root = document.object().value("transport").toObject();
		const auto video = document.object().value("video").toObject();
		auto payload = tgcalls::GroupJoinResponsePayload();
		payload.serverVideoBandwidthProbingSsrc = uint32_t(
			video.value("server_sources").toArray().at(0).toDouble());
		payload.ufrag = ReadJsonString(root, "ufrag");
		payload.pwd = ReadJsonString(root, "pwd");
		const auto prints = root.value("fingerprints").toArray();
		const auto candidates = root.value("candidates").toArray();
		for (const auto &print : prints) {
			const auto object = print.toObject();
			payload.fingerprints.push_back({
				.hash = ReadJsonString(object, "hash"),
				.setup = ReadJsonString(object, "setup"),
				.fingerprint = ReadJsonString(object, "fingerprint"),
			});
		}
		for (const auto &candidate : candidates) {
			const auto object = candidate.toObject();
			payload.candidates.push_back({
				.port = ReadJsonString(object, "port"),
				.protocol = ReadJsonString(object, "protocol"),
				.network = ReadJsonString(object, "network"),
				.generation = ReadJsonString(object, "generation"),
				.id = ReadJsonString(object, "id"),
				.component = ReadJsonString(object, "component"),
				.foundation = ReadJsonString(object, "foundation"),
				.priority = ReadJsonString(object, "priority"),
				.ip = ReadJsonString(object, "ip"),
				.type = ReadJsonString(object, "type"),
				.tcpType = ReadJsonString(object, "tcpType"),
				.relAddr = ReadJsonString(object, "relAddr"),
				.relPort = ReadJsonString(object, "relPort"),
			});
		}

		parseCommonVideoFields(video);

		setInstanceMode(InstanceMode::Rtc);
		_instance->setJoinResponsePayload(payload, {});
	});
}

void GroupCall::parseCommonVideoFields(const QJsonObject &root) {
	using namespace tgcalls;

	_commonVideoFields = std::make_unique<GroupParticipantDescription>();
	const auto raw = _commonVideoFields.get();

	const auto payloadTypes = root.value("payload-types").toArray();
	raw->videoPayloadTypes.reserve(payloadTypes.size());
	for (const auto &value : payloadTypes) {
		const auto inner = value.toObject();

		auto types = std::vector<GroupJoinPayloadVideoPayloadFeedbackType>();
		{
			const auto list = inner.value("rtcp-fbs").toArray();
			types.reserve(list.size());
			for (const auto &type : list) {
				const auto inside = type.toObject();
				types.push_back({
					.type = ReadJsonString(inside, "type"),
					.subtype = ReadJsonString(inside, "subtype"),
				});
			}
		}
		auto parameters = std::vector<std::pair<std::string, std::string>>();
		{
			const auto list = inner.value("parameters").toObject();
			parameters.reserve(list.size());
			for (auto i = list.begin(); i != list.end(); ++i) {
				parameters.push_back({
					i.key().toStdString(),
					i.value().toString().toStdString(),
				});
			}
		}
		raw->videoPayloadTypes.push_back({
			.id = uint32_t(inner.value("id").toDouble()),
			.name = ReadJsonString(inner, "name"),
			.clockrate = uint32_t(inner.value("clockrate").toDouble()),
			.channels = uint32_t(inner.value("channels").toDouble()),
			.feedbackTypes = std::move(types),
			.parameters = std::move(parameters),
		});
	}

	const auto extensionMap = root.value("rtp-hdrexts").toArray();
	raw->videoExtensionMap.reserve(extensionMap.size());
	for (const auto &extension : extensionMap) {
		const auto inner = extension.toObject();
		raw->videoExtensionMap.push_back({
			uint32_t(inner.value("id").toDouble()),
			ReadJsonString(inner, "uri"),
		});
	}
}

void GroupCall::handlePossibleDiscarded(const MTPDgroupCallDiscarded &data) {
	if (data.vid().v == _id) {
		LOG(("Call Info: Hangup after groupCallDiscarded."));
		_mySsrc = 0;
		hangup();
	}
}

void GroupCall::addParticipantsToInstance() {
	const auto real = lookupReal();
	if (!real
		|| (_instanceMode == InstanceMode::None)
		|| (_instanceMode == InstanceMode::Rtc && !_commonVideoFields)) {
		return;
	}
	for (const auto &participant : real->participants()) {
		prepareParticipantForAdding(participant);
	}
	addPreparedParticipants();
}

void GroupCall::prepareParticipantForAdding(
		const Data::GroupCallParticipant &participant) {
	const auto withVideo = _commonVideoFields && participant.videoParams;
	_preparedParticipants.push_back(withVideo
		? participant.videoParams->description
		: tgcalls::GroupParticipantDescription());
	auto &added = _preparedParticipants.back();
	if (withVideo) {
		added.videoSourceGroups = _commonVideoFields->videoSourceGroups;
		added.videoExtensionMap = _commonVideoFields->videoExtensionMap;
	}
	added.audioSsrc = participant.ssrc;
	_unresolvedSsrcs.remove(added.audioSsrc);
	for (const auto &group : added.videoSourceGroups) {
		for (const auto ssrc : group.ssrcs) {
			_unresolvedSsrcs.remove(ssrc);
		}
	}
}

void GroupCall::addPreparedParticipants() {
	_addPreparedParticipantsScheduled = false;
	if (!_preparedParticipants.empty()) {
		_instance->addParticipants(base::take(_preparedParticipants));
	}
	if (const auto real = lookupReal()) {
		if (!_unresolvedSsrcs.empty()) {
			real->resolveParticipants(base::take(_unresolvedSsrcs));
		}
	}
}

void GroupCall::addPreparedParticipantsDelayed() {
	if (_addPreparedParticipantsScheduled) {
		return;
	}
	_addPreparedParticipantsScheduled = true;
	crl::on_main(this, [=] { addPreparedParticipants(); });
}

void GroupCall::handleUpdate(const MTPUpdate &update) {
	update.match([&](const MTPDupdateGroupCall &data) {
		handleUpdate(data);
	}, [&](const MTPDupdateGroupCallParticipants &data) {
		handleUpdate(data);
	}, [](const auto &) {
		Unexpected("Type in Instance::applyGroupCallUpdateChecked.");
	});
}

void GroupCall::handleUpdate(const MTPDupdateGroupCall &data) {
	data.vcall().match([](const MTPDgroupCall &) {
	}, [&](const MTPDgroupCallDiscarded &data) {
		handlePossibleDiscarded(data);
	});
}

void GroupCall::handleUpdate(const MTPDupdateGroupCallParticipants &data) {
	const auto callId = data.vcall().match([](const auto &data) {
		return data.vid().v;
	});
	if (_id != callId) {
		return;
	}
	const auto state = _state.current();
	const auto joined = (state == State::Joined)
		|| (state == State::Connecting);
	for (const auto &participant : data.vparticipants().v) {
		participant.match([&](const MTPDgroupCallParticipant &data) {
			const auto isSelf = data.is_self()
				|| (data.is_min()
					&& peerFromMTP(data.vpeer()) == _joinAs->id);
			if (!isSelf) {
				applyOtherParticipantUpdate(data);
			} else if (joined) {
				applySelfUpdate(data);
			} else {
				_queuedSelfUpdates.push_back(participant);
			}
		});
	}
}

void GroupCall::applyQueuedSelfUpdates() {
	const auto weak = base::make_weak(this);
	while (weak
		&& !_queuedSelfUpdates.empty()
		&& (_state.current() == State::Joined
			|| _state.current() == State::Connecting)) {
		const auto update = _queuedSelfUpdates.front();
		_queuedSelfUpdates.erase(_queuedSelfUpdates.begin());
		update.match([&](const MTPDgroupCallParticipant &data) {
			applySelfUpdate(data);
		});
	}
}

void GroupCall::applySelfUpdate(const MTPDgroupCallParticipant &data) {
	if (data.is_left()) {
		if (data.vsource().v == _mySsrc) {
			// I was removed from the call, rejoin.
			LOG(("Call Info: "
				"Rejoin after got 'left' with my ssrc."));
			setState(State::Joining);
			rejoin();
		}
		return;
	} else if (data.vsource().v != _mySsrc) {
		if (!_mySsrcs.contains(data.vsource().v)) {
			// I joined from another device, hangup.
			LOG(("Call Info: "
				"Hangup after '!left' with ssrc %1, my %2."
				).arg(data.vsource().v
				).arg(_mySsrc));
			_mySsrc = 0;
			hangup();
		} else {
			LOG(("Call Info: "
				"Some old 'self' with '!left' and ssrc %1, my %2."
				).arg(data.vsource().v
				).arg(_mySsrc));
		}
		return;
	}
	if (data.is_muted() && !data.is_can_self_unmute()) {
		setMuted(data.vraise_hand_rating().value_or_empty()
			? MuteState::RaisedHand
			: MuteState::ForceMuted);
	} else if (_instanceMode == InstanceMode::Stream) {
		LOG(("Call Info: Rejoin after unforcemute in stream mode."));
		setState(State::Joining);
		rejoin();
	} else if (muted() == MuteState::ForceMuted
		|| muted() == MuteState::RaisedHand) {
		setMuted(MuteState::Muted);
		if (!_instanceTransitioning) {
			notifyAboutAllowedToSpeak();
		}
	} else if (data.is_muted() && muted() != MuteState::Muted) {
		setMuted(MuteState::Muted);
	}
}

void GroupCall::applyOtherParticipantUpdate(
		const MTPDgroupCallParticipant &data) {
	if (data.is_min()) {
		// No real information about mutedByMe or my custom volume.
		return;
	}
	const auto participantPeer = _peer->owner().peer(
		peerFromMTP(data.vpeer()));
	if (!LookupParticipant(_peer, _id, participantPeer)) {
		return;
	}
	_otherParticipantStateValue.fire(Group::ParticipantState{
		.peer = participantPeer,
		.volume = data.vvolume().value_or_empty(),
		.mutedByMe = data.is_muted_by_you(),
	});
}

void GroupCall::setupMediaDevices() {
	_mediaDevices->audioInputId(
	) | rpl::start_with_next([=](QString id) {
		_audioInputId = id;
		if (_instance) {
			_instance->setAudioInputDevice(id.toStdString());
		}
	}, _lifetime);

	_mediaDevices->audioOutputId(
	) | rpl::start_with_next([=](QString id) {
		_audioOutputId = id;
		if (_instance) {
			_instance->setAudioOutputDevice(id.toStdString());
		}
	}, _lifetime);

	_mediaDevices->videoInputId(
	) | rpl::start_with_next([=](QString id) {
		const auto usedCamera = !isScreenSharing();
		_videoInputId = id;
		if (_videoCapture && usedCamera) {
			_videoCapture->switchToDevice(_videoDeviceId.toStdString());
		}
	}, _lifetime);
	setupOutgoingVideo();
}

void GroupCall::setupOutgoingVideo() {
	_videoDeviceId = _videoInputId;
	static const auto hasDevices = [] {
		return !Webrtc::GetVideoInputList().empty();
	};
	const auto started = _videoOutgoing->state();
	if (!hasDevices()) {
		_videoOutgoing->setState(Webrtc::VideoState::Inactive);
	}
	_videoOutgoing->stateValue(
	) | rpl::start_with_next([=](Webrtc::VideoState state) {
		//if (state != Webrtc::VideoState::Inactive && !hasDevices()) {
			//_errors.fire({ ErrorType::NoCamera }); // #TODO videochats
			//_videoOutgoing->setState(Webrtc::VideoState::Inactive);
		//} else if (state != Webrtc::VideoState::Inactive
		//	&& _instance
		//	&& !_instance->supportsVideo()) {
		//	_errors.fire({ ErrorType::NotVideoCall });
		//	_videoOutgoing->setState(Webrtc::VideoState::Inactive);
		/*} else */if (state != Webrtc::VideoState::Inactive) {
			// Paused not supported right now.
			Assert(state == Webrtc::VideoState::Active);
			if (!_videoCapture) {
				_videoCapture = _delegate->groupCallGetVideoCapture(
					_videoDeviceId);
				_videoCapture->setOutput(_videoOutgoing->sink());
			} else {
				_videoCapture->switchToDevice(_videoDeviceId.toStdString());
			}
			if (_instance) {
				_instance->setVideoCapture(_videoCapture, nullptr);
			}
			_videoCapture->setState(tgcalls::VideoState::Active);
		} else if (_videoCapture) {
			_videoCapture->setState(tgcalls::VideoState::Inactive);
		}
	}, _lifetime);
}

void GroupCall::changeTitle(const QString &title) {
	const auto real = lookupReal();
	if (!real || real->title() == title) {
		return;
	}

	_api.request(MTPphone_EditGroupCallTitle(
		inputCall(),
		MTP_string(title)
	)).done([=](const MTPUpdates &result) {
		_peer->session().api().applyUpdates(result);
		_titleChanged.fire({});
	}).fail([=](const MTP::Error &error) {
	}).send();
}

void GroupCall::toggleRecording(bool enabled, const QString &title) {
	const auto real = lookupReal();
	if (!real) {
		return;
	}

	const auto already = (real->recordStartDate() != 0);
	if (already == enabled) {
		return;
	}

	if (!enabled) {
		_recordingStoppedByMe = true;
	}
	using Flag = MTPphone_ToggleGroupCallRecord::Flag;
	_api.request(MTPphone_ToggleGroupCallRecord(
		MTP_flags((enabled ? Flag::f_start : Flag(0))
			| (title.isEmpty() ? Flag(0) : Flag::f_title)),
		inputCall(),
		MTP_string(title)
	)).done([=](const MTPUpdates &result) {
		_peer->session().api().applyUpdates(result);
		_recordingStoppedByMe = false;
	}).fail([=](const MTP::Error &error) {
		_recordingStoppedByMe = false;
	}).send();
}

void GroupCall::ensureControllerCreated() {
	if (_instance) {
		return;
	}
	const auto &settings = Core::App().settings();

	const auto weak = base::make_weak(this);
	const auto myLevel = std::make_shared<tgcalls::GroupLevelValue>();
	_videoCall = true;
	tgcalls::GroupInstanceDescriptor descriptor = {
		.threads = tgcalls::StaticThreads::getThreads(),
		.config = tgcalls::GroupConfig{
		},
		.networkStateUpdated = [=](tgcalls::GroupNetworkState networkState) {
			crl::on_main(weak, [=] { setInstanceConnected(networkState); });
		},
		.audioLevelsUpdated = [=](const tgcalls::GroupLevelsUpdate &data) {
			const auto &updates = data.updates;
			if (updates.empty()) {
				return;
			} else if (updates.size() == 1 && !updates.front().ssrc) {
				const auto &value = updates.front().value;
				// Don't send many 0 while we're muted.
				if (myLevel->level == value.level
					&& myLevel->voice == value.voice) {
					return;
				}
				*myLevel = updates.front().value;
			}
			crl::on_main(weak, [=] { audioLevelsUpdated(data); });
		},
		.initialInputDeviceId = _audioInputId.toStdString(),
		.initialOutputDeviceId = _audioOutputId.toStdString(),
		.createAudioDeviceModule = Webrtc::AudioDeviceModuleCreator(
			settings.callAudioBackend()),
		.videoCapture = _videoCapture,
		.incomingVideoSourcesUpdated = [=](
				const std::vector<uint32_t> &ssrcs) {
			crl::on_main(weak, [=] {
				setVideoStreams(ssrcs);
			});
		},
		.participantDescriptionsRequired = [=](
				const std::vector<uint32_t> &ssrcs) {
			crl::on_main(weak, [=] {
				requestParticipantsInformation(ssrcs);
			});
		},
		.requestBroadcastPart = [=](
				int64_t time,
				int64_t period,
				std::function<void(tgcalls::BroadcastPart &&)> done) {
			auto result = std::make_shared<LoadPartTask>(
				weak,
				time,
				period,
				std::move(done));
			crl::on_main(weak, [=]() mutable {
				broadcastPartStart(std::move(result));
			});
			return result;
		},
		.videoContentType = tgcalls::VideoContentType::Generic,
	};
	if (Logs::DebugEnabled()) {
		auto callLogFolder = cWorkingDir() + qsl("DebugLogs");
		auto callLogPath = callLogFolder + qsl("/last_group_call_log.txt");
		auto callLogNative = QDir::toNativeSeparators(callLogPath);
#ifdef Q_OS_WIN
		descriptor.config.logPath.data = callLogNative.toStdWString();
#else // Q_OS_WIN
		const auto callLogUtf = QFile::encodeName(callLogNative);
		descriptor.config.logPath.data.resize(callLogUtf.size());
		ranges::copy(callLogUtf, descriptor.config.logPath.data.begin());
#endif // Q_OS_WIN
		QFile(callLogPath).remove();
		QDir().mkpath(callLogFolder);
	}

	LOG(("Call Info: Creating group instance"));
	_instance = std::make_unique<tgcalls::GroupInstanceCustomImpl>(
		std::move(descriptor));
	_videoStreamLarge.changes(
	) | rpl::start_with_next([=](uint32 ssrc) {
		_instance->setFullSizeVideoSsrc(ssrc);
		if (!ssrc) {
			_videoLargeTrack = nullptr;
			_videoLargeTrackWrap = nullptr;
			return;
		}
		if (!_videoLargeTrackWrap) {
			_videoLargeTrackWrap = std::make_unique<LargeTrack>();
			_videoLargeTrack = &_videoLargeTrackWrap->track;
		}
		_videoLargeTrackWrap->sink = Webrtc::CreateProxySink(
			_videoLargeTrackWrap->track.sink());
		_instance->addIncomingVideoOutput(ssrc, _videoLargeTrackWrap->sink);
	}, _lifetime);

	updateInstanceMuteState();
	updateInstanceVolumes();

	//raw->setAudioOutputDuckingEnabled(settings.callAudioDuckingEnabled());
}

void GroupCall::broadcastPartStart(std::shared_ptr<LoadPartTask> task) {
	const auto raw = task.get();
	const auto time = raw->time();
	const auto scale = raw->scale();
	const auto finish = [=](tgcalls::BroadcastPart &&part) {
		raw->done(std::move(part));
		_broadcastParts.erase(raw);
	};
	using Status = tgcalls::BroadcastPart::Status;
	const auto requestId = _api.request(MTPupload_GetFile(
		MTP_flags(0),
		MTP_inputGroupCallStream(
			inputCall(),
			MTP_long(time),
			MTP_int(scale)),
		MTP_int(0),
		MTP_int(128 * 1024)
	)).done([=](
			const MTPupload_File &result,
			const MTP::Response &response) {
		result.match([&](const MTPDupload_file &data) {
			const auto size = data.vbytes().v.size();
			auto bytes = std::vector<uint8_t>(size);
			memcpy(bytes.data(), data.vbytes().v.constData(), size);
			finish({
				.timestampMilliseconds = time,
				.responseTimestamp = TimestampFromMsgId(response.outerMsgId),
				.status = Status::Success,
				.oggData = std::move(bytes),
			});
		}, [&](const MTPDupload_fileCdnRedirect &data) {
			LOG(("Voice Chat Stream Error: fileCdnRedirect received."));
			finish({
				.timestampMilliseconds = time,
				.responseTimestamp = TimestampFromMsgId(response.outerMsgId),
				.status = Status::ResyncNeeded,
			});
		});
	}).fail([=](const MTP::Error &error, const MTP::Response &response) {
		if (error.type() == u"GROUPCALL_JOIN_MISSING"_q
			|| error.type() == u"GROUPCALL_FORBIDDEN"_q) {
			for (const auto &[task, part] : _broadcastParts) {
				_api.request(part.requestId).cancel();
			}
			setState(State::Joining);
			rejoin();
			return;
		}
		const auto status = (MTP::IsFloodError(error)
			|| error.type() == u"TIME_TOO_BIG"_q)
			? Status::NotReady
			: Status::ResyncNeeded;
		finish({
			.timestampMilliseconds = time,
			.responseTimestamp = TimestampFromMsgId(response.outerMsgId),
			.status = status,
		});
	}).handleAllErrors().toDC(
		MTP::groupCallStreamDcId(_broadcastDcId)
	).send();
	_broadcastParts.emplace(raw, LoadingPart{ std::move(task), requestId });
}

void GroupCall::broadcastPartCancel(not_null<LoadPartTask*> task) {
	const auto i = _broadcastParts.find(task);
	if (i != _broadcastParts.end()) {
		_api.request(i->second.requestId).cancel();
		_broadcastParts.erase(i);
	}
}

void GroupCall::requestParticipantsInformation(
		const std::vector<uint32_t> &ssrcs) {
	const auto real = lookupReal();
	if (!real || (_instanceMode == InstanceMode::None)) {
		for (const auto ssrc : ssrcs) {
			_unresolvedSsrcs.emplace(ssrc);
		}
		return;
	}

	const auto &existing = real->participants();
	for (const auto ssrc : ssrcs) {
		const auto byAudio = real->participantPeerByAudioSsrc(ssrc);
		const auto participantPeer = byAudio
			? byAudio
			: real->participantPeerByVideoSsrc(ssrc);
		if (!participantPeer) {
			_unresolvedSsrcs.emplace(ssrc);
			continue;
		}
		const auto i = ranges::find(
			existing,
			not_null{ participantPeer },
			&Data::GroupCall::Participant::peer);
		Assert(i != end(existing));

		prepareParticipantForAdding(*i);
	}
	addPreparedParticipants();
}

void GroupCall::setVideoStreams(const std::vector<std::uint32_t> &ssrcs) {
	const auto large = _videoStreamLarge.current();
	auto newLarge = large;
	if (large && !ranges::contains(ssrcs, large)) {
		newLarge = 0;
		_videoStreamPinned = 0;
	}
	auto removed = _videoStreamSsrcs;
	for (const auto ssrc : ssrcs) {
		const auto i = removed.find(ssrc);
		const auto videoMuted = _videoMuted.contains(ssrc);
		if (i != end(removed)) {
			removed.erase(i);
		} else {
			_videoStreamSsrcs.emplace(ssrc);
			if (!videoMuted) {
				_streamsVideoUpdated.fire({ ssrc, true });
			}
		}
	}
	if (!newLarge) {
		_videoStreamLarge = chooseLargeVideoSsrc();
	}
	for (const auto ssrc : removed) {
		if (!_videoMuted.contains(ssrc)) {
			_streamsVideoUpdated.fire({ ssrc, false });
		}
	}
}

uint32 GroupCall::chooseLargeVideoSsrc() const {
	const auto real = lookupReal();
	if (!real) {
		return 0;
	}
	auto anySsrc = uint32(0);
	auto lastSpokeVoiceSsrc = uint32(0);
	auto lastSpokeAnythingSsrc = uint32(0);
	const auto &participants = real->participants();
	for (const auto ssrc : _videoStreamSsrcs) {
		if (_videoMuted.contains(ssrc)) {
			continue;
		}
		const auto &participants = real->participants();
		const auto i = ranges::find(
			participants,
			ssrc,
			&Data::GroupCallParticipant::ssrc);
		if (i != end(participants)) {
			if (!lastSpokeVoiceSsrc && i->speaking) {
				lastSpokeVoiceSsrc = ssrc;
			}
			if (!lastSpokeAnythingSsrc && i->sounding) {
				lastSpokeAnythingSsrc = ssrc;
			}
			if (!anySsrc) {
				anySsrc = ssrc;
			}
		}
	}
	return lastSpokeVoiceSsrc
		? lastSpokeVoiceSsrc
		: lastSpokeAnythingSsrc
		? lastSpokeAnythingSsrc
		: anySsrc;
}

void GroupCall::updateInstanceMuteState() {
	Expects(_instance != nullptr);

	const auto state = muted();
	_instance->setIsMuted(state != MuteState::Active
		&& state != MuteState::PushToTalk);
}

void GroupCall::updateInstanceVolumes() {
	const auto real = lookupReal();
	if (!real) {
		return;
	}

	const auto &participants = real->participants();
	for (const auto &participant : participants) {
		const auto setVolume = participant.mutedByMe
			|| (participant.volume != Group::kDefaultVolume);
		if (setVolume && participant.ssrc) {
			_instance->setVolume(
				participant.ssrc,
				(participant.mutedByMe
					? 0.
					: (participant.volume / float64(Group::kDefaultVolume))));
		}
	}
}

void GroupCall::audioLevelsUpdated(const tgcalls::GroupLevelsUpdate &data) {
	Expects(!data.updates.empty());

	auto check = false;
	auto checkNow = false;
	const auto now = crl::now();
	for (const auto &[ssrcOrZero, value] : data.updates) {
		const auto ssrc = ssrcOrZero ? ssrcOrZero : _mySsrc;
		const auto level = value.level;
		const auto voice = value.voice;
		const auto me = (ssrc == _mySsrc);
		_levelUpdates.fire(LevelUpdate{
			.ssrc = ssrc,
			.value = level,
			.voice = voice,
			.me = me
		});
		if (level <= kSpeakLevelThreshold) {
			continue;
		}
		if (me
			&& voice
			&& (!_lastSendProgressUpdate
				|| _lastSendProgressUpdate + kUpdateSendActionEach < now)) {
			_lastSendProgressUpdate = now;
			_peer->session().sendProgressManager().update(
				_history,
				Api::SendProgressType::Speaking);
		}

		check = true;
		const auto i = _lastSpoke.find(ssrc);
		if (i == _lastSpoke.end()) {
			_lastSpoke.emplace(ssrc, Data::LastSpokeTimes{
				.anything = now,
				.voice = voice ? now : 0,
			});
			checkNow = true;
		} else {
			if ((i->second.anything + kCheckLastSpokeInterval / 3 <= now)
				|| (voice
					&& i->second.voice + kCheckLastSpokeInterval / 3 <= now)) {
				checkNow = true;
			}
			i->second.anything = now;
			if (voice) {
				i->second.voice = now;
			}
		}
	}
	if (checkNow) {
		checkLastSpoke();
	} else if (check && !_lastSpokeCheckTimer.isActive()) {
		_lastSpokeCheckTimer.callEach(kCheckLastSpokeInterval / 2);
	}
}

void GroupCall::checkLastSpoke() {
	const auto real = lookupReal();
	if (!real) {
		return;
	}

	auto hasRecent = false;
	const auto now = crl::now();
	auto list = base::take(_lastSpoke);
	for (auto i = list.begin(); i != list.end();) {
		const auto [ssrc, when] = *i;
		if (when.anything + kCheckLastSpokeInterval >= now) {
			hasRecent = true;
			++i;
		} else {
			i = list.erase(i);
		}
		real->applyLastSpoke(ssrc, when, now);
	}
	_lastSpoke = std::move(list);

	if (!hasRecent) {
		_lastSpokeCheckTimer.cancel();
	} else if (!_lastSpokeCheckTimer.isActive()) {
		_lastSpokeCheckTimer.callEach(kCheckLastSpokeInterval / 3);
	}
}

void GroupCall::checkJoined() {
	if (state() != State::Connecting || !_id || !_mySsrc) {
		return;
	}
	auto sources = QVector<MTPint>(1, MTP_int(_mySsrc));
	if (_screencastSsrc) {
		sources.push_back(MTP_int(_screencastSsrc));
	}
	_api.request(MTPphone_CheckGroupCall(
		inputCall(),
		MTP_vector<MTPint>(std::move(sources))
	)).done([=](const MTPVector<MTPint> &result) {
		if (!ranges::contains(result.v, MTP_int(_mySsrc))) {
			LOG(("Call Info: Rejoin after no _mySsrc in checkGroupCall."));
			rejoin();
		} else if (state() == State::Connecting) {
			_checkJoinedTimer.callOnce(kCheckJoinedTimeout);
		}
		if (_screencastSsrc
			&& !ranges::contains(result.v, MTP_int(_screencastSsrc))) {
			LOG(("Call Info: "
				"Rejoin presentation after _screencastSsrc not found."));
			// #TODO calls
		}
	}).fail([=](const MTP::Error &error) {
 		LOG(("Call Info: Full rejoin after error '%1' in checkGroupCall."
			).arg(error.type()));
		rejoin();
		if (_screencastSsrc) {
			// #TODO calls
		}
	}).send();
}

void GroupCall::setInstanceConnected(
		tgcalls::GroupNetworkState networkState) {
	const auto inTransit = networkState.isTransitioningFromBroadcastToRtc;
	const auto instanceState = !networkState.isConnected
		? InstanceState::Disconnected
		: inTransit
		? InstanceState::TransitionToRtc
		: InstanceState::Connected;
	const auto connected = (instanceState != InstanceState::Disconnected);
	if (_instanceState.current() == instanceState
		&& _instanceTransitioning == inTransit) {
		return;
	}
	const auto nowCanSpeak = connected
		&& _instanceTransitioning
		&& !inTransit
		&& (muted() == MuteState::Muted);
	_instanceTransitioning = inTransit;
	_instanceState = instanceState;
	if (state() == State::Connecting && connected) {
		setState(State::Joined);
	} else if (state() == State::Joined && !connected) {
		setState(State::Connecting);
	}
	if (nowCanSpeak) {
		notifyAboutAllowedToSpeak();
	}
	if (!_hadJoinedState && state() == State::Joined) {
		checkFirstTimeJoined();
	}
}

void GroupCall::checkFirstTimeJoined() {
	if (_hadJoinedState || state() != State::Joined) {
		return;
	}
	_hadJoinedState = true;
	applyGlobalShortcutChanges();
	_delegate->groupCallPlaySound(Delegate::GroupCallSound::Started);
}

void GroupCall::notifyAboutAllowedToSpeak() {
	if (!_hadJoinedState) {
		return;
	}
	_delegate->groupCallPlaySound(
		Delegate::GroupCallSound::AllowedToSpeak);
	_allowedToSpeakNotifications.fire({});
}

void GroupCall::setInstanceMode(InstanceMode mode) {
	Expects(_instance != nullptr);

	_instanceMode = mode;

	using Mode = tgcalls::GroupConnectionMode;
	_instance->setConnectionMode([&] {
		switch (_instanceMode) {
		case InstanceMode::None: return Mode::GroupConnectionModeNone;
		case InstanceMode::Rtc: return Mode::GroupConnectionModeRtc;
		case InstanceMode::Stream: return Mode::GroupConnectionModeBroadcast;
		}
		Unexpected("Mode in GroupCall::setInstanceMode.");
	}(), true);
}

void GroupCall::maybeSendMutedUpdate(MuteState previous) {
	// Send Active <-> !Active or ForceMuted <-> RaisedHand changes.
	const auto now = muted();
	if ((previous == MuteState::Active && now == MuteState::Muted)
		|| (now == MuteState::Active
			&& (previous == MuteState::Muted
				|| previous == MuteState::PushToTalk))) {
		sendSelfUpdate(SendUpdateType::Mute);
	} else if ((now == MuteState::ForceMuted
		&& previous == MuteState::RaisedHand)
		|| (now == MuteState::RaisedHand
			&& previous == MuteState::ForceMuted)) {
		sendSelfUpdate(SendUpdateType::RaiseHand);
	}
}

void GroupCall::sendSelfUpdate(SendUpdateType type) {
	_api.request(_updateMuteRequestId).cancel();
	using Flag = MTPphone_EditGroupCallParticipant::Flag;
	_updateMuteRequestId = _api.request(MTPphone_EditGroupCallParticipant(
		MTP_flags((type == SendUpdateType::RaiseHand)
			? Flag::f_raise_hand
			: (type == SendUpdateType::VideoMuted)
			? Flag::f_video_muted
			: Flag::f_muted),
		inputCall(),
		_joinAs->input,
		MTP_bool(muted() != MuteState::Active),
		MTP_int(100000), // volume
		MTP_bool(muted() == MuteState::RaisedHand),
		MTP_bool(_videoOutgoing->state() != Webrtc::VideoState::Active)
	)).done([=](const MTPUpdates &result) {
		_updateMuteRequestId = 0;
		_peer->session().api().applyUpdates(result);
	}).fail([=](const MTP::Error &error) {
		_updateMuteRequestId = 0;
		if (error.type() == u"GROUPCALL_FORBIDDEN"_q) {
			LOG(("Call Info: Rejoin after error '%1' in editGroupCallMember."
				).arg(error.type()));
			rejoin();
		}
	}).send();
}

void GroupCall::pinVideoStream(uint32 ssrc) {
	if (!ssrc || streamsVideo(ssrc)) {
		_videoStreamPinned = ssrc;
		if (ssrc) {
			_videoStreamLarge = ssrc;
		}
	}
}

void GroupCall::setCurrentAudioDevice(bool input, const QString &deviceId) {
	if (input) {
		_mediaDevices->switchToAudioInput(deviceId);
	} else {
		_mediaDevices->switchToAudioOutput(deviceId);
	}
}

void GroupCall::setCurrentVideoDevice(const QString &deviceId) {
	_mediaDevices->switchToVideoInput(deviceId);
}

void GroupCall::toggleMute(const Group::MuteRequest &data) {
	if (data.locallyOnly) {
		applyParticipantLocally(data.peer, data.mute, std::nullopt);
	} else {
		editParticipant(data.peer, data.mute, std::nullopt);
	}
}

void GroupCall::changeVolume(const Group::VolumeRequest &data) {
	if (data.locallyOnly) {
		applyParticipantLocally(data.peer, false, data.volume);
	} else {
		editParticipant(data.peer, false, data.volume);
	}
}

void GroupCall::editParticipant(
		not_null<PeerData*> participantPeer,
		bool mute,
		std::optional<int> volume) {
	const auto participant = LookupParticipant(_peer, _id, participantPeer);
	if (!participant) {
		return;
	}
	applyParticipantLocally(participantPeer, mute, volume);

	using Flag = MTPphone_EditGroupCallParticipant::Flag;
	const auto flags = Flag::f_muted
		| (volume.has_value() ? Flag::f_volume : Flag(0));
	_api.request(MTPphone_EditGroupCallParticipant(
		MTP_flags(flags),
		inputCall(),
		participantPeer->input,
		MTP_bool(mute),
		MTP_int(std::clamp(volume.value_or(0), 1, Group::kMaxVolume)),
		MTPBool(), // raise_hand
		MTPBool() // video_muted
	)).done([=](const MTPUpdates &result) {
		_peer->session().api().applyUpdates(result);
	}).fail([=](const MTP::Error &error) {
		if (error.type() == u"GROUPCALL_FORBIDDEN"_q) {
			LOG(("Call Info: Rejoin after error '%1' in editGroupCallMember."
				).arg(error.type()));
			rejoin();
		}
	}).send();
}

std::variant<int, not_null<UserData*>> GroupCall::inviteUsers(
		const std::vector<not_null<UserData*>> &users) {
	const auto real = lookupReal();
	if (!real) {
		return 0;
	}
	const auto owner = &_peer->owner();
	const auto &invited = owner->invitedToCallUsers(_id);
	const auto &participants = real->participants();
	auto &&toInvite = users | ranges::views::filter([&](
			not_null<UserData*> user) {
		return !invited.contains(user) && !ranges::contains(
			participants,
			user,
			&Data::GroupCall::Participant::peer);
	});

	auto count = 0;
	auto slice = QVector<MTPInputUser>();
	auto result = std::variant<int, not_null<UserData*>>(0);
	slice.reserve(kMaxInvitePerSlice);
	const auto sendSlice = [&] {
		count += slice.size();
		_api.request(MTPphone_InviteToGroupCall(
			inputCall(),
			MTP_vector<MTPInputUser>(slice)
		)).done([=](const MTPUpdates &result) {
			_peer->session().api().applyUpdates(result);
		}).send();
		slice.clear();
	};
	for (const auto user : users) {
		if (!count && slice.empty()) {
			result = user;
		}
		owner->registerInvitedToCallUser(_id, _peer, user);
		slice.push_back(user->inputUser);
		if (slice.size() == kMaxInvitePerSlice) {
			sendSlice();
		}
	}
	if (count != 0 || slice.size() != 1) {
		result = int(count + slice.size());
	}
	if (!slice.empty()) {
		sendSlice();
	}
	return result;
}

auto GroupCall::ensureGlobalShortcutManager()
-> std::shared_ptr<GlobalShortcutManager> {
	if (!_shortcutManager) {
		_shortcutManager = base::CreateGlobalShortcutManager();
	}
	return _shortcutManager;
}

void GroupCall::applyGlobalShortcutChanges() {
	auto &settings = Core::App().settings();
	if (!settings.groupCallPushToTalk()
		|| settings.groupCallPushToTalkShortcut().isEmpty()
		|| !base::GlobalShortcutsAvailable()
		|| !base::GlobalShortcutsAllowed()) {
		_shortcutManager = nullptr;
		_pushToTalk = nullptr;
		return;
	}
	ensureGlobalShortcutManager();
	const auto shortcut = _shortcutManager->shortcutFromSerialized(
		settings.groupCallPushToTalkShortcut());
	if (!shortcut) {
		settings.setGroupCallPushToTalkShortcut(QByteArray());
		settings.setGroupCallPushToTalk(false);
		Core::App().saveSettingsDelayed();
		_shortcutManager = nullptr;
		_pushToTalk = nullptr;
		return;
	}
	if (_pushToTalk) {
		if (shortcut->serialize() == _pushToTalk->serialize()) {
			return;
		}
		_shortcutManager->stopWatching(_pushToTalk);
	}
	_pushToTalk = shortcut;
	_shortcutManager->startWatching(_pushToTalk, [=](bool pressed) {
		pushToTalk(
			pressed,
			Core::App().settings().groupCallPushToTalkDelay());
	});
}

void GroupCall::pushToTalk(bool pressed, crl::time delay) {
	if (muted() == MuteState::ForceMuted
		|| muted() == MuteState::RaisedHand
		|| muted() == MuteState::Active) {
		return;
	} else if (pressed) {
		_pushToTalkCancelTimer.cancel();
		setMuted(MuteState::PushToTalk);
	} else if (delay) {
		_pushToTalkCancelTimer.callOnce(delay);
	} else {
		pushToTalkCancel();
	}
}

void GroupCall::pushToTalkCancel() {
	_pushToTalkCancelTimer.cancel();
	if (muted() == MuteState::PushToTalk) {
		setMuted(MuteState::Muted);
	}
}

auto GroupCall::otherParticipantStateValue() const
-> rpl::producer<Group::ParticipantState> {
	return _otherParticipantStateValue.events();
}

MTPInputGroupCall GroupCall::inputCall() const {
	Expects(_id != 0);

	return MTP_inputGroupCall(
		MTP_long(_id),
		MTP_long(_accessHash));
}

void GroupCall::destroyController() {
	if (_instance) {
		DEBUG_LOG(("Call Info: Destroying call controller.."));
		_instance.reset();
		DEBUG_LOG(("Call Info: Call controller destroyed."));
	}
}

} // namespace Calls