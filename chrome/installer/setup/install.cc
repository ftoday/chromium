// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/installer/setup/install.h"

#include <shlobj.h>
#include <time.h>
#include <winuser.h>

#include <string>

#include "base/command_line.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "base/path_service.h"
#include "base/string_util.h"
#include "base/stringprintf.h"
#include "base/utf_string_conversions.h"
#include "base/win/shortcut.h"
#include "base/win/windows_version.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/installer/setup/setup_constants.h"
#include "chrome/installer/setup/install_worker.h"
#include "chrome/installer/util/auto_launch_util.h"
#include "chrome/installer/util/browser_distribution.h"
#include "chrome/installer/util/create_reg_key_work_item.h"
#include "chrome/installer/util/delete_after_reboot_helper.h"
#include "chrome/installer/util/google_update_constants.h"
#include "chrome/installer/util/helper.h"
#include "chrome/installer/util/install_util.h"
#include "chrome/installer/util/master_preferences.h"
#include "chrome/installer/util/master_preferences_constants.h"
#include "chrome/installer/util/set_reg_value_work_item.h"
#include "chrome/installer/util/shell_util.h"
#include "chrome/installer/util/util_constants.h"
#include "chrome/installer/util/work_item_list.h"

// Build-time generated include file.
#include "registered_dlls.h"  // NOLINT

using installer::InstallerState;
using installer::InstallationState;
using installer::Product;

namespace {

void LogShortcutOperation(ShellUtil::ChromeShortcutLocation location,
                          BrowserDistribution* dist,
                          const ShellUtil::ChromeShortcutProperties& properties,
                          ShellUtil::ChromeShortcutOperation operation,
                          bool failed) {
  // SHORTCUT_UPDATE_EXISTING should not be used at install and thus this method
  // does not handle logging a message for it.
  DCHECK(operation != ShellUtil::SHORTCUT_UPDATE_EXISTING);
  std::string message;
  if (failed)
    message.append("Failed: ");
  message.append(operation == ShellUtil::SHORTCUT_CREATE_ALWAYS ?
      "Creating " : "Overwriting ");
  if (failed && operation == ShellUtil::SHORTCUT_REPLACE_EXISTING)
    message.append("(maybe the shortcut doesn't exist?) ");
  message.append((properties.level == ShellUtil::CURRENT_USER) ? "per-user " :
                                                                 "all-users ");
  switch (location) {
    case ShellUtil::SHORTCUT_DESKTOP:
      message.append("Desktop ");
      break;
    case ShellUtil::SHORTCUT_QUICK_LAUNCH:
      message.append("Quick Launch ");
      break;
    case ShellUtil::SHORTCUT_START_MENU:
      message.append("Start menu ");
      break;
    default:
      NOTREACHED();
  }

  message.push_back('"');
  if (properties.has_shortcut_name())
    message.append(UTF16ToUTF8(properties.shortcut_name));
  else
    message.append(UTF16ToUTF8(dist->GetAppShortCutName()));
  message.push_back('"');

  message.append(" shortcut to ");
  message.append(UTF16ToUTF8(properties.chrome_exe.value()));
  if (properties.has_arguments())
    message.append(UTF16ToUTF8(properties.arguments));

  if (properties.pin_to_taskbar &&
      base::win::GetVersion() >= base::win::VERSION_WIN7) {
    message.append(" and pinning to the taskbar.");
  } else {
    message.push_back('.');
  }

  if (failed)
    LOG(WARNING) << message;
  else
    VLOG(1) << message;
}

void ExecuteAndLogShortcutOperation(
    ShellUtil::ChromeShortcutLocation location,
    BrowserDistribution* dist,
    const ShellUtil::ChromeShortcutProperties& properties,
    ShellUtil::ChromeShortcutOperation operation) {
  LogShortcutOperation(location, dist, properties, operation, false);
  if (!ShellUtil::CreateOrUpdateChromeShortcut(location, dist, properties,
                                               operation)) {
    LogShortcutOperation(location, dist, properties, operation, true);
  }
}

void AddChromeToMediaPlayerList() {
  string16 reg_path(installer::kMediaPlayerRegPath);
  // registry paths can also be appended like file system path
  reg_path.push_back(FilePath::kSeparators[0]);
  reg_path.append(installer::kChromeExe);
  VLOG(1) << "Adding Chrome to Media player list at " << reg_path;
  scoped_ptr<WorkItem> work_item(WorkItem::CreateCreateRegKeyWorkItem(
      HKEY_LOCAL_MACHINE, reg_path));

  // if the operation fails we log the error but still continue
  if (!work_item.get()->Do())
    LOG(ERROR) << "Could not add Chrome to media player inclusion list.";
}

// Copy master preferences file provided to installer, in the same folder
// as chrome.exe so Chrome first run can find it. This function will be called
// only on the first install of Chrome.
void CopyPreferenceFileForFirstRun(const InstallerState& installer_state,
                                   const FilePath& prefs_source_path) {
  FilePath prefs_dest_path(installer_state.target_path().AppendASCII(
      installer::kDefaultMasterPrefs));
  if (!file_util::CopyFile(prefs_source_path, prefs_dest_path)) {
    VLOG(1) << "Failed to copy master preferences from:"
            << prefs_source_path.value() << " gle: " << ::GetLastError();
  }
}

// Returns true if the current process is running on the interactive window
// station.  This cares not whether the input desktop is the default or not
// (i.e., the screen saver is running, or what have you).
bool IsInteractiveProcess() {
  static const wchar_t kWinSta0[] = L"WinSta0";
  HWINSTA window_station = ::GetProcessWindowStation();
  if (window_station == NULL) {
    PLOG(ERROR) << "Failed to get window station";
    return false;
  }

  // Make the buffer one char longer and zero it to be certain it's terminated.
  wchar_t name[arraysize(kWinSta0) + 1] = {};
  DWORD buffer_length = sizeof(kWinSta0);
  DWORD name_length = 0;
  return (GetUserObjectInformation(window_station, UOI_NAME, &name[0],
                                   buffer_length, &name_length) &&
          name_length == buffer_length &&
          lstrcmpi(kWinSta0, name) == 0);
}

// This function installs a new version of Chrome to the specified location.
//
// setup_path: Path to the executable (setup.exe) as it will be copied
//           to Chrome install folder after install is complete
// archive_path: Path to the archive (chrome.7z) as it will be copied
//               to Chrome install folder after install is complete
// src_path: the path that contains a complete and unpacked Chrome package
//           to be installed.
// temp_path: the path of working directory used during installation. This path
//            does not need to exist.
// new_version: new Chrome version that needs to be installed
// current_version: returns the current active version (if any)
//
// This function makes best effort to do installation in a transactional
// manner. If failed it tries to rollback all changes on the file system
// and registry. For example, if package exists before calling the
// function, it rolls back all new file and directory changes under
// package. If package does not exist before calling the function
// (typical new install), the function creates package during install
// and removes the whole directory during rollback.
installer::InstallStatus InstallNewVersion(
    const InstallationState& original_state,
    const InstallerState& installer_state,
    const FilePath& setup_path,
    const FilePath& archive_path,
    const FilePath& src_path,
    const FilePath& temp_path,
    const Version& new_version,
    scoped_ptr<Version>* current_version) {
  DCHECK(current_version);

  installer_state.UpdateStage(installer::BUILDING);

  current_version->reset(installer_state.GetCurrentVersion(original_state));
  scoped_ptr<WorkItemList> install_list(WorkItem::CreateWorkItemList());

  AddInstallWorkItems(original_state,
                      installer_state,
                      setup_path,
                      archive_path,
                      src_path,
                      temp_path,
                      current_version->get(),
                      new_version,
                      install_list.get());

  FilePath new_chrome_exe(
      installer_state.target_path().Append(installer::kChromeNewExe));

  installer_state.UpdateStage(installer::EXECUTING);

  if (!install_list->Do()) {
    installer_state.UpdateStage(installer::ROLLINGBACK);
    installer::InstallStatus result =
        file_util::PathExists(new_chrome_exe) && current_version->get() &&
        new_version.Equals(*current_version->get()) ?
        installer::SAME_VERSION_REPAIR_FAILED :
        installer::INSTALL_FAILED;
    LOG(ERROR) << "Install failed, rolling back... result: " << result;
    install_list->Rollback();
    LOG(ERROR) << "Rollback complete. ";
    return result;
  }

  installer_state.UpdateStage(installer::REFRESHING_POLICY);

  installer::RefreshElevationPolicy();

  if (!current_version->get()) {
    VLOG(1) << "First install of version " << new_version.GetString();
    return installer::FIRST_INSTALL_SUCCESS;
  }

  if (new_version.Equals(**current_version)) {
    VLOG(1) << "Install repaired of version " << new_version.GetString();
    return installer::INSTALL_REPAIRED;
  }

  if (new_version.CompareTo(**current_version) > 0) {
    if (file_util::PathExists(new_chrome_exe)) {
      VLOG(1) << "Version updated to " << new_version.GetString()
              << " while running " << (*current_version)->GetString();
      return installer::IN_USE_UPDATED;
    }
    VLOG(1) << "Version updated to " << new_version.GetString();
    return installer::NEW_VERSION_UPDATED;
  }

  LOG(ERROR) << "Not sure how we got here while updating"
             << ", new version: " << new_version.GetString()
             << ", old version: " << (*current_version)->GetString();

  return installer::INSTALL_FAILED;
}

void CleanupLegacyShortcuts(const InstallerState& installer_state,
                            BrowserDistribution* dist) {
  ShellUtil::ShellChange shortcut_level = installer_state.system_install() ?
      ShellUtil::SYSTEM_LEVEL : ShellUtil::CURRENT_USER;
  FilePath uninstall_shortcut_path;
  ShellUtil::GetShortcutPath(ShellUtil::SHORTCUT_START_MENU, dist,
                             shortcut_level, &uninstall_shortcut_path);
  uninstall_shortcut_path = uninstall_shortcut_path.Append(
      dist->GetUninstallLinkName() + installer::kLnkExt);
  file_util::Delete(uninstall_shortcut_path, false);
}

}  // end namespace

namespace installer {

void EscapeXmlAttributeValueInSingleQuotes(string16* att_value) {
  ReplaceChars(*att_value, L"&", L"&amp;", att_value);
  ReplaceChars(*att_value, L"'", L"&apos;", att_value);
  ReplaceChars(*att_value, L"<", L"&lt;", att_value);
}

bool CreateVisualElementsManifest(const FilePath& src_path,
                                  const Version& version) {
  // Construct the relative path to the versioned VisualElements directory.
  string16 elements_dir(ASCIIToUTF16(version.GetString()));
  elements_dir.push_back(FilePath::kSeparators[0]);
  elements_dir.append(installer::kVisualElements);

  // Some distributions of Chromium may not include visual elements. Only
  // proceed if this distribution does.
  if (!file_util::PathExists(src_path.Append(elements_dir))) {
    VLOG(1) << "No visual elements found, not writing "
            << installer::kVisualElementsManifest << " to " << src_path.value();
    return true;
  } else {
    // A printf_p-style format string for generating the visual elements
    // manifest. Required arguments, in order, are:
    //   - Localized display name for the product.
    //   - Relative path to the VisualElements directory.
    static const char kManifestTemplate[] =
        "<Application>\r\n"
        "  <VisualElements\r\n"
        "      DisplayName='%1$ls'\r\n"
        "      Logo='%2$ls\\Logo.png'\r\n"
        "      SmallLogo='%2$ls\\SmallLogo.png'\r\n"
        "      ForegroundText='light'\r\n"
        "      BackgroundColor='#323232'>\r\n"
        "    <DefaultTile ShowName='allLogos'/>\r\n"
        "    <SplashScreen Image='%2$ls\\splash-620x300.png'/>\r\n"
        "  </VisualElements>\r\n"
        "</Application>";

    const string16 manifest_template(ASCIIToUTF16(kManifestTemplate));

    BrowserDistribution* dist = BrowserDistribution::GetSpecificDistribution(
        BrowserDistribution::CHROME_BROWSER);
    // TODO(grt): http://crbug.com/75152 Write a reference to a localized
    // resource for |display_name|.
    string16 display_name(dist->GetAppShortCutName());
    EscapeXmlAttributeValueInSingleQuotes(&display_name);

    // Fill the manifest with the desired values.
    string16 manifest16(base::StringPrintf(manifest_template.c_str(),
                                           display_name.c_str(),
                                           elements_dir.c_str()));

    // Write the manifest to |src_path|.
    const std::string manifest(UTF16ToUTF8(manifest16));
    if (file_util::WriteFile(
            src_path.Append(installer::kVisualElementsManifest),
            manifest.c_str(), manifest.size())) {
      VLOG(1) << "Successfully wrote " << installer::kVisualElementsManifest
              << " to " << src_path.value();
      return true;
    } else {
      PLOG(ERROR) << "Error writing " << installer::kVisualElementsManifest
                  << " to " << src_path.value();
      return false;
    }
  }
}

void CreateOrUpdateShortcuts(const InstallerState& installer_state,
                             const FilePath& setup_exe,
                             const Product& product,
                             InstallShortcutOperation install_operation,
                             bool alternate_desktop_shortcut) {
  // TODO(tommi): Change this function to use WorkItemList.
  DCHECK(product.is_chrome());

  BrowserDistribution* dist = product.distribution();
  const FilePath chrome_exe(
      installer_state.target_path().Append(installer::kChromeExe));
  ShellUtil::ShellChange install_level =
      installer_state.system_install() ? ShellUtil::SYSTEM_LEVEL :
                                         ShellUtil::CURRENT_USER;

  // The default operation on update is to overwrite shortcuts with the
  // currently desired properties, but do so only for shortcuts that still
  // exist.
  ShellUtil::ChromeShortcutOperation shortcut_operation =
      ShellUtil::SHORTCUT_REPLACE_EXISTING;

  // |base_properties|: The basic properties to set on every shortcut installed
  // (to be refined on a per-shortcut basis).
  ShellUtil::ChromeShortcutProperties base_properties(install_level);
  base_properties.set_chrome_exe(chrome_exe);

  // If |install_operation| is INSTALL_SHORTCUT_CREATE_ALL, create optional
  // shortcuts (Desktop and Quick Launch) immediately; otherwise delay their
  // creation until first run.
  if (install_operation == INSTALL_SHORTCUT_CREATE_ALL)
    shortcut_operation = ShellUtil::SHORTCUT_CREATE_ALWAYS;

  ShellUtil::ChromeShortcutProperties desktop_properties(base_properties);
  if (alternate_desktop_shortcut)
    desktop_properties.set_shortcut_name(dist->GetAlternateApplicationName());
  ExecuteAndLogShortcutOperation(
      ShellUtil::SHORTCUT_DESKTOP, dist, desktop_properties,
      shortcut_operation);

  ExecuteAndLogShortcutOperation(
      ShellUtil::SHORTCUT_QUICK_LAUNCH, dist, base_properties,
      shortcut_operation);
  if (installer_state.system_install() &&
      shortcut_operation == ShellUtil::SHORTCUT_CREATE_ALWAYS) {
    // On system-level installs, also create the quick launch shortcut for this
    // user (as the all-users shortcut created is in "Default User" and only
    // affects new users).
    ShellUtil::ChromeShortcutProperties user_ql_properties(base_properties);
    user_ql_properties.level = ShellUtil::CURRENT_USER;
    ExecuteAndLogShortcutOperation(
        ShellUtil::SHORTCUT_QUICK_LAUNCH, dist, user_ql_properties,
        shortcut_operation);
  }

  // Turn on shortcut creation for mandatory shortcuts if requested.
  if (install_operation == INSTALL_SHORTCUT_CREATE_MANDATORY)
    shortcut_operation = ShellUtil::SHORTCUT_CREATE_ALWAYS;

  ShellUtil::ChromeShortcutProperties start_menu_properties(base_properties);
  // IMPORTANT: Only the default (no arguments and default browserappid) browser
  // shortcut in the Start menu (Start screen on Win8+) should be  made dual
  // mode.
  start_menu_properties.set_dual_mode(true);
  if (shortcut_operation == ShellUtil::SHORTCUT_CREATE_ALWAYS)
    start_menu_properties.set_pin_to_taskbar(true);
  ExecuteAndLogShortcutOperation(
      ShellUtil::SHORTCUT_START_MENU, dist, start_menu_properties,
      shortcut_operation);
}

void RegisterChromeOnMachine(const InstallerState& installer_state,
                             const Product& product,
                             bool make_chrome_default) {
  DCHECK(product.is_chrome());

  // Try to add Chrome to Media Player shim inclusion list. We don't do any
  // error checking here because this operation will fail if user doesn't
  // have admin rights and we want to ignore the error.
  AddChromeToMediaPlayerList();

  // Make Chrome the default browser if desired when possible. Otherwise, only
  // register it with Windows.
  BrowserDistribution* dist = product.distribution();
  const string16 chrome_exe(
      installer_state.target_path().Append(installer::kChromeExe).value());
  VLOG(1) << "Registering Chrome as browser: " << chrome_exe;
  if (make_chrome_default) {
    if (ShellUtil::CanMakeChromeDefaultUnattended()) {
      int level = ShellUtil::CURRENT_USER;
      if (installer_state.system_install())
        level = level | ShellUtil::SYSTEM_LEVEL;
      ShellUtil::MakeChromeDefault(dist, level, chrome_exe, true);
    } else if (IsInteractiveProcess() &&
               ShellUtil::GetChromeDefaultState() != ShellUtil::IS_DEFAULT) {
      ShellUtil::ShowMakeChromeDefaultSystemUI(dist, chrome_exe);
    } else {
      ShellUtil::RegisterChromeBrowser(dist, chrome_exe, string16(), false);
    }
  } else {
    ShellUtil::RegisterChromeBrowser(dist, chrome_exe, string16(), false);
  }

  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

InstallStatus InstallOrUpdateProduct(
    const InstallationState& original_state,
    const InstallerState& installer_state,
    const FilePath& setup_path,
    const FilePath& archive_path,
    const FilePath& install_temp_path,
    const FilePath& prefs_path,
    const MasterPreferences& prefs,
    const Version& new_version) {
  FilePath src_path(install_temp_path);
  src_path = src_path.Append(kInstallSourceDir).Append(kInstallSourceChromeDir);

  // TODO(robertshield): Removing the pending on-reboot moves should be done
  // elsewhere.
  // TODO(erikwright): Understand why this is Chrome Frame only and whether
  // it also applies to App Host. Shouldn't it apply to any multi-install too?
  const Products& products = installer_state.products();
  DCHECK(products.size());
  if (installer_state.FindProduct(BrowserDistribution::CHROME_FRAME)) {
    // Make sure that we don't end up deleting installed files on next reboot.
    if (!RemoveFromMovesPendingReboot(
            installer_state.target_path().value().c_str())) {
      LOG(ERROR) << "Error accessing pending moves value.";
    }
  }

  // Create VisualElementManifest.xml in |src_path| (if required) so that it
  // looks as if it had been extracted from the archive when calling
  // InstallNewVersion() below.
  installer_state.UpdateStage(installer::CREATING_VISUAL_MANIFEST);
  CreateVisualElementsManifest(src_path, new_version);

  scoped_ptr<Version> existing_version;
  InstallStatus result = InstallNewVersion(original_state, installer_state,
      setup_path, archive_path, src_path, install_temp_path, new_version,
      &existing_version);

  // TODO(robertshield): Everything below this line should instead be captured
  // by WorkItems.
  if (!InstallUtil::GetInstallReturnCode(result)) {
    installer_state.UpdateStage(installer::UPDATING_CHANNELS);

    // Update the modifiers on the channel values for the product(s) being
    // installed and for the binaries in case of multi-install.
    installer_state.UpdateChannels();

    installer_state.UpdateStage(installer::COPYING_PREFERENCES_FILE);

    if (result == FIRST_INSTALL_SUCCESS && !prefs_path.empty())
      CopyPreferenceFileForFirstRun(installer_state, prefs_path);

    // Currently this only creates shortcuts for Chrome, but for other products
    // we might want to create shortcuts.
    const Product* chrome_install =
        installer_state.FindProduct(BrowserDistribution::CHROME_BROWSER);
    if (chrome_install) {
      installer_state.UpdateStage(installer::CREATING_SHORTCUTS);

      BrowserDistribution* dist = chrome_install->distribution();
      CleanupLegacyShortcuts(installer_state, dist);

      bool create_all_shortcuts = false;
      prefs.GetBool(master_preferences::kCreateAllShortcuts,
                    &create_all_shortcuts);
      bool alt_shortcut = false;
      prefs.GetBool(master_preferences::kAltShortcutText, &alt_shortcut);

      InstallShortcutOperation install_operation =
          INSTALL_SHORTCUT_REPLACE_EXISTING;
      if (create_all_shortcuts) {
        install_operation = INSTALL_SHORTCUT_CREATE_ALL;
      } else if (result == installer::FIRST_INSTALL_SUCCESS ||
                 result == installer::INSTALL_REPAIRED) {
        // On new and repaired installs, always create Start Menu, taskbar, and
        // uninstall shortcuts (i.e. even if they were previously deleted by the
        // user).
        install_operation = INSTALL_SHORTCUT_CREATE_MANDATORY;
      }

      FilePath setup_exe(installer_state.GetInstallerDirectory(new_version)
          .Append(setup_path.BaseName()));
      CreateOrUpdateShortcuts(installer_state, setup_exe, *chrome_install,
                              install_operation, alt_shortcut);

      bool make_chrome_default = false;
      prefs.GetBool(master_preferences::kMakeChromeDefault,
                    &make_chrome_default);

      // If this is not the user's first Chrome install, but they have chosen
      // Chrome to become their default browser on the download page, we must
      // force it here because the master_preferences file will not get copied
      // into the build.
      bool force_chrome_default_for_user = false;
      if (result == NEW_VERSION_UPDATED ||
          result == INSTALL_REPAIRED) {
        prefs.GetBool(master_preferences::kMakeChromeDefaultForUser,
                      &force_chrome_default_for_user);
      }

      installer_state.UpdateStage(installer::REGISTERING_CHROME);

      RegisterChromeOnMachine(installer_state, *chrome_install,
          make_chrome_default || force_chrome_default_for_user);

      if (result == FIRST_INSTALL_SUCCESS) {
        installer_state.UpdateStage(installer::CONFIGURE_AUTO_LAUNCH);

        // Add auto-launch key if specified in master preferences.
        bool auto_launch_chrome = false;
        prefs.GetBool(
            installer::master_preferences::kAutoLaunchChrome,
            &auto_launch_chrome);
        if (auto_launch_chrome) {
          auto_launch_util::EnableForegroundStartAtLogin(
              ASCIIToUTF16(chrome::kInitialProfile),
              installer_state.target_path());
        }
      }
    }

    installer_state.UpdateStage(installer::REMOVING_OLD_VERSIONS);

    installer_state.RemoveOldVersionDirectories(
        new_version,
        existing_version.get(),
        install_temp_path);
  }

  return result;
}

void HandleOsUpgradeForBrowser(const InstallerState& installer_state,
                               const Product& chrome,
                               const FilePath& setup_exe) {
  DCHECK(chrome.is_chrome());
  // Upon upgrading to Windows 8, we need to fix Chrome shortcuts and register
  // Chrome, so that Metro Chrome would work if Chrome is the default browser.
  if (base::win::GetVersion() >= base::win::VERSION_WIN8) {
    VLOG(1) << "Updating and registering shortcuts.";
    CreateOrUpdateShortcuts(
        installer_state, setup_exe, chrome, INSTALL_SHORTCUT_REPLACE_EXISTING,
        false);
    RegisterChromeOnMachine(installer_state, chrome, false);
  }
}

}  // namespace installer
