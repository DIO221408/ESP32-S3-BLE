#include <string.h>

#include "app_context.h"

void app_context_init(app_context_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->inmp411_noise_ema_db = -1.0f;
}
