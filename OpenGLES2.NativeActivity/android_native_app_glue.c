/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Apache 라이선스 2.0 버전(이하 "라이선스")에 따라 라이선스가 부여됩니다.
 * 라이선스를 준수하지 않으면 이 파일을 사용할 수 없습니다.
 * 라이선스의 사본은
 *
 *      http://www.apache.org/licenses/LICENSE-2.0에서 얻을 수 있습니다.
 *
 * 적용 가능한 법률에 따라 필요하거나 서면으로 동의하지 않는 이상
 * 라이선스에 따라 배포되는 소프트웨어는 "있는 그대로",
 * 명시적 또는 묵시적이든 어떠한 유형의 보증이나 조건 없이 배포됩니다.
 * 라이선스에 따른 특정 언어의 권한 및 제한에 대한 내용은
 * 라이선스를 참조하세요.
 *
 */

#include <unistd.h>
#include <sys/resource.h>

static void free_saved_state (struct android_app * android_app)
{
	pthread_mutex_lock (&android_app->mutex);

	if (android_app->savedState != nullptr)
	{
		free (android_app->savedState);
		android_app->savedState = nullptr;
		android_app->savedStateSize = 0;
	}

	pthread_mutex_unlock (&android_app->mutex);
}

int8_t android_app_read_cmd (struct android_app * android_app)
{
	int8_t cmd;

	if (read (android_app->msgread, &cmd, sizeof (cmd)) == sizeof (cmd))
	{
		switch (cmd)
		{
		case APP_CMD_SAVE_STATE:
			free_saved_state (android_app);
			break;
		default:
			break;
		}

		return cmd;
	}
	else
		loge ("No data on command pipe!");

	return -1;
}

static void print_cur_config (struct android_app * android_app)
{
	char lang [2], country [2];

	AConfiguration_getLanguage (android_app->config, lang);
	AConfiguration_getCountry (android_app->config, country);

	logv ("Config: mcc=%d mnc=%d lang=%c%c cnt=%c%c orien=%d touch=%d dens=%d "
		"keys=%d nav=%d keysHid=%d navHid=%d sdk=%d size=%d long=%d "
		"modetype=%d modenight=%d",
		AConfiguration_getMcc (android_app->config),
		AConfiguration_getMnc (android_app->config),
		lang [0], lang [1], country [0], country [1],
		AConfiguration_getOrientation (android_app->config),
		AConfiguration_getTouchscreen (android_app->config),
		AConfiguration_getDensity (android_app->config),
		AConfiguration_getKeyboard (android_app->config),
		AConfiguration_getNavigation (android_app->config),
		AConfiguration_getKeysHidden (android_app->config),
		AConfiguration_getNavHidden (android_app->config),
		AConfiguration_getSdkVersion (android_app->config),
		AConfiguration_getScreenSize (android_app->config),
		AConfiguration_getScreenLong (android_app->config),
		AConfiguration_getUiModeType (android_app->config),
		AConfiguration_getUiModeNight (android_app->config));
}

void android_app_pre_exec_cmd (struct android_app * android_app, int8_t cmd)
{
	switch (cmd)
	{
	case APP_CMD_INPUT_CHANGED:
		logv ("APP_CMD_INPUT_CHANGED\n");
		pthread_mutex_lock (&android_app->mutex);

		if (android_app->inputQueue != nullptr)
			AInputQueue_detachLooper (android_app->inputQueue);

		android_app->inputQueue = android_app->pendingInputQueue;

		if (android_app->inputQueue != nullptr)
		{
			logv ("Attaching input queue to looper");
			AInputQueue_attachLooper (android_app->inputQueue, android_app->looper, LOOPER_ID_INPUT, nullptr, &android_app->inputPollSource);
		}

		pthread_cond_broadcast (&android_app->cond);
		pthread_mutex_unlock (&android_app->mutex);
		break;

	case APP_CMD_INIT_WINDOW:
		logv ("APP_CMD_INIT_WINDOW\n");
		pthread_mutex_lock (&android_app->mutex);

		android_app->window = android_app->pendingWindow;

		pthread_cond_broadcast (&android_app->cond);
		pthread_mutex_unlock (&android_app->mutex);
		break;

	case APP_CMD_TERM_WINDOW:
		logv ("APP_CMD_TERM_WINDOW\n");
		pthread_cond_broadcast (&android_app->cond);
		break;

	case APP_CMD_RESUME:
	case APP_CMD_START:
	case APP_CMD_PAUSE:
	case APP_CMD_STOP:
		logv ("activityState=%d\n", cmd);
		pthread_mutex_lock (&android_app->mutex);

		android_app->activityState = cmd;

		pthread_cond_broadcast (&android_app->cond);
		pthread_mutex_unlock (&android_app->mutex);
		break;

	case APP_CMD_CONFIG_CHANGED:
		logv ("APP_CMD_CONFIG_CHANGED\n");
		AConfiguration_fromAssetManager (android_app->config, android_app->activity->assetManager);
		print_cur_config (android_app);
		break;

	case APP_CMD_DESTROY:
		logv ("APP_CMD_DESTROY\n");
		android_app->destroyRequested = 1;
		break;

	default:
		break;
	}
}

void android_app_post_exec_cmd (struct android_app * android_app, int8_t cmd)
{
	switch (cmd)
	{
	case APP_CMD_TERM_WINDOW:
		logv ("APP_CMD_TERM_WINDOW\n");
		pthread_mutex_lock (&android_app->mutex);

		android_app->window = nullptr;

		pthread_cond_broadcast (&android_app->cond);
		pthread_mutex_unlock (&android_app->mutex);
		break;

	case APP_CMD_SAVE_STATE:
		logv ("APP_CMD_SAVE_STATE\n");
		pthread_mutex_lock (&android_app->mutex);

		android_app->stateSaved = 1;

		pthread_cond_broadcast (&android_app->cond);
		pthread_mutex_unlock (&android_app->mutex);
		break;

	case APP_CMD_RESUME:
		free_saved_state (android_app);
		break;

	default:
		break;
	}
}

static void android_app_destroy (struct android_app * android_app)
{
	logv ("android_app_destroy!");
	free_saved_state (android_app);
	pthread_mutex_lock (&android_app->mutex);

	if (android_app->inputQueue != nullptr)
		AInputQueue_detachLooper (android_app->inputQueue);

	AConfiguration_delete (android_app->config);

	android_app->destroyed = 1;

	pthread_cond_broadcast (&android_app->cond);
	pthread_mutex_unlock (&android_app->mutex);
}

static void process_input (struct android_app * app, struct android_poll_source * source)
{
	AInputEvent * event = nullptr;

	while (AInputQueue_getEvent (app->inputQueue, &event) >= 0)
	{
		logv ("New input event: type=%d\n", AInputEvent_getType (event));

		if (AInputQueue_preDispatchEvent (app->inputQueue, event))
			continue;

		int32_t handled = 0;

		if (app->onInputEvent != nullptr)
			handled = app->onInputEvent (app, event);

		AInputQueue_finishEvent (app->inputQueue, event, handled);
	}
}

static void process_cmd (struct android_app * app, struct android_poll_source * source)
{
	int8_t cmd = android_app_read_cmd (app);

	android_app_pre_exec_cmd (app, cmd);

	if (app->onAppCmd != nullptr)
		app->onAppCmd (app, cmd);

	android_app_post_exec_cmd (app, cmd);
}

static void * android_app_entry (void * param)
{
	struct android_app * android_app = (struct android_app *) param;

	android_app->config = AConfiguration_new ();
	AConfiguration_fromAssetManager (android_app->config, android_app->activity->assetManager);

	print_cur_config (android_app);

	android_app->cmdPollSource.id = LOOPER_ID_MAIN;
	android_app->cmdPollSource.app = android_app;
	android_app->cmdPollSource.process = process_cmd;
	android_app->inputPollSource.id = LOOPER_ID_INPUT;
	android_app->inputPollSource.app = android_app;
	android_app->inputPollSource.process = process_input;

	ALooper * looper = ALooper_prepare (ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);

	ALooper_addFd (looper, android_app->msgread, LOOPER_ID_MAIN, ALOOPER_EVENT_INPUT, nullptr, &android_app->cmdPollSource);

	android_app->looper = looper;

	pthread_mutex_lock (&android_app->mutex);

	android_app->running = 1;

	pthread_cond_broadcast (&android_app->cond);
	pthread_mutex_unlock (&android_app->mutex);
	android_main (android_app);
	android_app_destroy (android_app);

	return nullptr;
}

// --------------------------------------------------------------------
// Native Activity 상호 작용(주 스레드에서 호출됨)
// --------------------------------------------------------------------

static struct android_app * android_app_create (ANativeActivity * activity, void * savedState, size_t savedStateSize)
{
	struct android_app * android_app = (struct android_app *) malloc (sizeof (struct android_app));

	memset (android_app, 0, sizeof (struct android_app));

	android_app->activity = activity;

	pthread_mutex_init (&android_app->mutex, nullptr);
	pthread_cond_init (&android_app->cond, nullptr);

	if (savedState != nullptr)
	{
		android_app->savedState = malloc (savedStateSize);
		android_app->savedStateSize = savedStateSize;
		memcpy (android_app->savedState, savedState, savedStateSize);
	}

	int msgpipe [2];

	if (pipe (msgpipe))
	{
		loge ("could not create pipe: %s", strerror (errno));
		return nullptr;
	}

	android_app->msgread = msgpipe [0];
	android_app->msgwrite = msgpipe [1];

	pthread_attr_t attr;

	pthread_attr_init (&attr);
	pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);
	pthread_create (&android_app->thread, &attr, android_app_entry, android_app);

	pthread_mutex_lock (&android_app->mutex);

	while (!android_app->running)
		pthread_cond_wait (&android_app->cond, &android_app->mutex);

	pthread_mutex_unlock (&android_app->mutex);

	return android_app;
}

static void android_app_write_cmd (struct android_app * android_app, int8_t cmd)
{
	if (write (android_app->msgwrite, &cmd, sizeof (cmd)) != sizeof (cmd))
		loge ("Failure writing android_app cmd: %s\n", strerror (errno));
}

static void android_app_set_input (struct android_app * android_app, AInputQueue * inputQueue)
{
	pthread_mutex_lock (&android_app->mutex);

	android_app->pendingInputQueue = inputQueue;

	android_app_write_cmd (android_app, APP_CMD_INPUT_CHANGED);

	while (android_app->inputQueue != android_app->pendingInputQueue)
		pthread_cond_wait (&android_app->cond, &android_app->mutex);
	
	pthread_mutex_unlock (&android_app->mutex);
}

static void android_app_set_window (struct android_app * android_app, ANativeWindow * window)
{
	pthread_mutex_lock (&android_app->mutex);

	if (android_app->pendingWindow != nullptr)
		android_app_write_cmd (android_app, APP_CMD_TERM_WINDOW);

	android_app->pendingWindow = window;

	if (window != nullptr)
		android_app_write_cmd (android_app, APP_CMD_INIT_WINDOW);

	while (android_app->window != android_app->pendingWindow)
		pthread_cond_wait (&android_app->cond, &android_app->mutex);

	pthread_mutex_unlock (&android_app->mutex);
}

static void android_app_set_activity_state (struct android_app * android_app, int8_t cmd)
{
	pthread_mutex_lock (&android_app->mutex);
	android_app_write_cmd (android_app, cmd);

	while (android_app->activityState != cmd)
		pthread_cond_wait (&android_app->cond, &android_app->mutex);
	
	pthread_mutex_unlock (&android_app->mutex);
}

static void android_app_free (struct android_app * android_app)
{
	pthread_mutex_lock (&android_app->mutex);
	android_app_write_cmd (android_app, APP_CMD_DESTROY);

	while (!android_app->destroyed)
		pthread_cond_wait (&android_app->cond, &android_app->mutex);
	
	pthread_mutex_unlock (&android_app->mutex);

	close (android_app->msgread);
	close (android_app->msgwrite);
	pthread_cond_destroy (&android_app->cond);
	pthread_mutex_destroy (&android_app->mutex);
	free (android_app);
}

static void onDestroy (ANativeActivity * activity)
{
	logv ("Destroy: %p\n", activity);
	android_app_free ((struct android_app *) activity->instance);
}

static void onStart (ANativeActivity * activity)
{
	logv ("Start: %p\n", activity);
	android_app_set_activity_state ((struct android_app *) activity->instance, APP_CMD_START);
}

static void onResume (ANativeActivity * activity)
{
	logv ("Resume: %p\n", activity);
	android_app_set_activity_state ((struct android_app *) activity->instance, APP_CMD_RESUME);
}

static void * onSaveInstanceState (ANativeActivity * activity, size_t * outLen)
{
	struct android_app * android_app = (struct android_app *) activity->instance;
	void * savedState = nullptr;

	logv ("SaveInstanceState: %p\n", activity);
	pthread_mutex_lock (&android_app->mutex);

	android_app->stateSaved = 0;
	android_app_write_cmd (android_app, APP_CMD_SAVE_STATE);

	while (!android_app->stateSaved)
		pthread_cond_wait (&android_app->cond, &android_app->mutex);

	if (android_app->savedState != nullptr)
	{
		savedState = android_app->savedState;
		* outLen = android_app->savedStateSize;
		android_app->savedState = nullptr;
		android_app->savedStateSize = 0;
	}

	pthread_mutex_unlock (&android_app->mutex);

	return savedState;
}

static void onPause (ANativeActivity * activity)
{
	logv ("Pause: %p\n", activity);
	android_app_set_activity_state ((struct android_app *) activity->instance, APP_CMD_PAUSE);
}

static void onStop (ANativeActivity * activity)
{
	logv ("Stop: %p\n", activity);
	android_app_set_activity_state ((struct android_app *) activity->instance, APP_CMD_STOP);
}

static void onConfigurationChanged (ANativeActivity * activity)
{
	struct android_app * android_app = (struct android_app *) activity->instance;
	logv ("ConfigurationChanged: %p\n", activity);
	android_app_write_cmd (android_app, APP_CMD_CONFIG_CHANGED);
}

static void onLowMemory (ANativeActivity * activity)
{
	struct android_app * android_app = (struct android_app *) activity->instance;
	logv ("LowMemory: %p\n", activity);
	android_app_write_cmd (android_app, APP_CMD_LOW_MEMORY);
}

static void onWindowFocusChanged (ANativeActivity * activity, int focused)
{
	logv ("WindowFocusChanged: %p -- %d\n", activity, focused);
	android_app_write_cmd ((struct android_app *) activity->instance, focused ? APP_CMD_GAINED_FOCUS : APP_CMD_LOST_FOCUS);
}

static void onNativeWindowCreated (ANativeActivity * activity, ANativeWindow * window)
{
	logv ("NativeWindowCreated: %p -- %p\n", activity, window);
	android_app_set_window ((struct android_app *) activity->instance, window);
}

static void onNativeWindowDestroyed (ANativeActivity * activity, ANativeWindow * window)
{
	logv ("NativeWindowDestroyed: %p -- %p\n", activity, window);
	android_app_set_window ((struct android_app *) activity->instance, nullptr);
}

static void onInputQueueCreated (ANativeActivity * activity, AInputQueue * queue)
{
	logv ("InputQueueCreated: %p -- %p\n", activity, queue);
	android_app_set_input ((struct android_app *) activity->instance, queue);
}

static void onInputQueueDestroyed (ANativeActivity * activity, AInputQueue * queue)
{
	logv ("InputQueueDestroyed: %p -- %p\n", activity, queue);
	android_app_set_input ((struct android_app *) activity->instance, nullptr);
}

void ANativeActivity_onCreate (ANativeActivity * activity, void * savedState, size_t savedStateSize)
{
	logv ("Creating: %p\n", activity);

	activity->callbacks->onDestroy = onDestroy;
	activity->callbacks->onStart = onStart;
	activity->callbacks->onResume = onResume;
	activity->callbacks->onSaveInstanceState = onSaveInstanceState;
	activity->callbacks->onPause = onPause;
	activity->callbacks->onStop = onStop;
	activity->callbacks->onConfigurationChanged = onConfigurationChanged;
	activity->callbacks->onLowMemory = onLowMemory;
	activity->callbacks->onWindowFocusChanged = onWindowFocusChanged;
	activity->callbacks->onNativeWindowCreated = onNativeWindowCreated;
	activity->callbacks->onNativeWindowDestroyed = onNativeWindowDestroyed;
	activity->callbacks->onInputQueueCreated = onInputQueueCreated;
	activity->callbacks->onInputQueueDestroyed = onInputQueueDestroyed;
	activity->instance = android_app_create (activity, savedState, savedStateSize);
}
