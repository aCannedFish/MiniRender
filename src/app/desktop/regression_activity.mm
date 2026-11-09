#include "regression_activity.hpp"

#import <Foundation/Foundation.h>

RegressionActivity::RegressionActivity(bool enabled) {
    if (enabled) {
        id token = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                              reason:@"MiniRender automated regression"];
        token_ = (__bridge_retained void*)token;
    }
}

RegressionActivity::~RegressionActivity() {
    if (token_) {
        id token = (__bridge_transfer id)token_;
        [[NSProcessInfo processInfo] endActivity:token];
    }
}
