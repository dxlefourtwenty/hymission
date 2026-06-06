#pragma once

#include "overview_controller.hpp"

namespace hymission::niri_scrolling_detail {

extern bool stripSnapshotSingleWorkspaceOnly;

SP<Hyprutils::Animation::SAnimationPropertyConfig> windowsMoveAnimationConfig();

void armTwoColumnSwapTrace(const PHLWORKSPACE& workspace);
bool twoColumnSwapTraceActive(const PHLWORKSPACE& workspace);
bool consumeTwoColumnSwapPreviewTrace(const PHLWORKSPACE& workspace);
void armPendingTwoColumnSwapRepair(const PHLWORKSPACE& workspace);
void clearPendingTwoColumnSwapRepair(const PHLWORKSPACE& workspace);
bool consumePendingTwoColumnSwapRepair(const PHLWORKSPACE& workspace);
bool retileFillTransformActiveForWindow(const PHLWINDOW& window);
void armRetileFillTransformForWindow(const PHLWINDOW& window);
bool isActiveController(const OverviewController* controller);
bool shouldWrapWorkspaceIds(WORKSPACEID targetId, WORKSPACEID currentId);

} // namespace hymission::niri_scrolling_detail
