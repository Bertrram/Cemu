/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

// A TCP server on 127.0.0.1 that lets another program on this machine place figures
// on the emulated Skylanders portal, as the Emulated USB Devices window does.
// It is off unless <SkylandersIPC><enabled> is set in settings.xml.
namespace nsyshid::SkylanderPortalIPC
{
	// starts the server for the running title, if it is enabled and the portal is emulated
	void Start();
	void Stop();

	// while hushed the title reads no controller input, as while the input settings window is open
	bool IsInputHushed();
} // namespace nsyshid::SkylanderPortalIPC
