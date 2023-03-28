
// This file is licensed under the BSD-3 Clause License
// Copyright 2022 © Charlotte Belanger

#include "injector.h"

#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <dirent.h>
#include <dlfcn.h>
#include <os/log.h>
#include <mach-o/dyld.h>

static int compare(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

os_log_t eklog;

#if TARGET_OS_OSX
#define TWEAKS_DIRECTORY "/Library/TweakInject/"
#else
#define TWEAKS_DIRECTORY "/var/jb/usr/lib/TweakInject/"
#define MOBILESAFETY_PATH "/var/jb/usr/lib/ellekit/MobileSafety.dylib"
#endif

CFStringRef copyAndLowercaseCFString(CFStringRef input) {
    CFMutableStringRef mutableCopy = CFStringCreateMutableCopy(NULL, 0, input);
    CFStringLowercase(mutableCopy, NULL);
    return mutableCopy;
}

char* drop_last_n_chars(const char* str, size_t n) {
    size_t len = strlen(str);

    if (n >= len) {
        return NULL;  // invalid input, n is too large
    }

    char* new_str = malloc(len - n + 1);  // allocate memory for the new string
    if (new_str == NULL) {
        return NULL;  // allocation failed
    }

    strncpy(new_str, str, len - n);  // copy the first len - n characters
    new_str[len - n] = '\0';  // terminate the new string

    return new_str;
}

char* append_str(const char* str, const char* append_str) {
    size_t str_len = strlen(str);
    size_t append_str_len = strlen(append_str);

    char* new_str = malloc(str_len + append_str_len + 1);  // allocate memory for the new string
    if (new_str == NULL) {
        return NULL;  // allocation failed
    }

    strcpy(new_str, str);  // copy the original string to the new string
    strcat(new_str, append_str);  // append the new string to the end
    new_str[str_len + append_str_len] = '\0';  // terminate the new string

    return new_str;
}

char *get_last_path_component(const char *path)
{
    char *last_component = strrchr(path, '/');
    if (last_component == NULL) {
        return NULL;
    }
    return last_component + 1;
}

#warning "Add bundle checks, needs choicy code"
static bool tweak_needinject(const char* orig_path) {
    
    CFStringRef plistPath;
    CFURLRef url;
    CFDataRef data;
    CFPropertyListRef plist;
    
    char* path = append_str(orig_path, ".plist");
        
    plistPath = CFStringCreateWithCString(kCFAllocatorDefault, path, kCFStringEncodingUTF8);
            
    if (!!access(path, F_OK)) {
        free(path);
        CFRelease(plistPath);
        return false;
    }
    
    free(path);
    
    url = CFURLCreateWithFileSystemPath(kCFAllocatorSystemDefault, plistPath, kCFURLPOSIXPathStyle, false);
        
    if (url && CFURLCreateDataAndPropertiesFromResource(kCFAllocatorSystemDefault, url, &data, NULL, NULL, NULL)) {
        plist = CFPropertyListCreateWithData(kCFAllocatorSystemDefault, data, kCFPropertyListImmutable, NULL, NULL);
        CFRelease(data);
    } else {
        if (url) {
            CFRelease(url);
        }
        CFRelease(plistPath);
        return false;
    }
    CFRelease(url);
    CFRelease(plistPath);
                            
    CFDictionaryRef filter = CFDictionaryGetValue(plist, CFSTR("Filter"));
    CFArrayRef bundles = CFDictionaryGetValue(filter, CFSTR("Bundles"));
    
    if (bundles) {
        for (CFIndex i = 0; i < CFArrayGetCount(bundles); i++) {
            CFStringRef id = CFArrayGetValueAtIndex(bundles, i);
            CFStringRef lowercased = copyAndLowercaseCFString(id);
            if (CFBundleGetBundleWithIdentifier(id) || CFBundleGetBundleWithIdentifier(lowercased)) {
                CFRelease(lowercased);
                goto success;
            }
            CFRelease(lowercased);
        }
    }
    
    CFArrayRef classes = CFDictionaryGetValue(filter, CFSTR("Classes"));
    
    if (classes) {
        for (CFIndex i = 0; i < CFArrayGetCount(classes); i++) {
            CFStringRef id = CFArrayGetValueAtIndex(classes, i);
                                            
            char* str = malloc(CFStringGetLength(id)+1);
            
            CFStringGetCString(id, str, CFStringGetLength(id)+1, kCFStringEncodingASCII);
                        
            if (objc_getClass(str)) {
                free(str);
                goto success;
            }
            
            free(str);
        }
    }
    
    CFArrayRef executables = CFDictionaryGetValue(filter, CFSTR("Executables"));

    if (executables) {
        
        char executable[1024];
        uint32_t size = sizeof(path);

        if (_NSGetExecutablePath(executable, &size) == 0) {
            
            for (CFIndex i = 0; i < CFArrayGetCount(executables); i++) {
                CFStringRef id = CFArrayGetValueAtIndex(executables, i);

                char* str = malloc(CFStringGetLength(id)+1);

                CFStringGetCString(id, str, CFStringGetLength(id)+1, kCFStringEncodingASCII);

                printf("opening %s\n", str);
                
                if (strcmp(str, get_last_path_component(executable))) {
                    free(str);
                    goto success;
                }

                free(str);
            }
        } else if (CFBundleGetMainBundle()) {
            for (CFIndex i = 0; i < CFArrayGetCount(executables); i++) {
                CFStringRef id = CFArrayGetValueAtIndex(executables, i);

                char *name_str = NULL;
                CFBundleRef bundle = CFBundleGetMainBundle();
                CFURLRef url = CFBundleCopyExecutableURL(bundle);
                if (url) {
                    CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
                    CFStringRef file_name = CFURLCopyLastPathComponent(url);
                    if (file_name) {
                        if (CFStringCompare(file_name, id, kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
                            CFRelease(file_name);
                            CFRelease(path);
                            CFRelease(url);
                            free(name_str);
                            goto success;
                        }
                        CFRelease(file_name);
                    }
                    CFRelease(path);
                    CFRelease(url);
                }
                
                free(name_str);

            }
        }
    }
    
    CFRelease(plist);
    return false;
    
success:
    CFRelease(plist);
    return true;
}

static void tweaks_iterate() {
    DIR *dir;
    struct dirent *ent;
    char **files;
    int i, n;

    dir = opendir(TWEAKS_DIRECTORY);
    if (dir == NULL) {
        perror("opendir");
        exit(EXIT_FAILURE);
    }

    n = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG) {
            n++;
        }
    }

    rewinddir(dir);
    
    if (n == 0) {
        return;
    }

    files = malloc(n * sizeof(char *));
    if (files == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    i = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG) {
            files[i] = strdup(ent->d_name);
            i++;
        }
    }

    qsort(files, n, sizeof(char *), compare);

    for (i = 0; i < n; ++i) {
        if (!!strstr(files[i], ".dylib")) {
            
            char *full_path = append_str(TWEAKS_DIRECTORY, files[i]);
            
            char* plist = drop_last_n_chars(full_path, 6);
            
            bool ret = tweak_needinject(plist);
            if (ret) {
                dlopen(full_path, RTLD_NOW);
                
                char* err = dlerror();
                
                if (err) {
                    os_log_with_type(eklog, OS_LOG_TYPE_ERROR, "[libinjector] Got dlopen error: %s", err);
                }
            }
            free(full_path);
            free(plist);
        }
        free(files[i]);
    }
    
    free(files);

    closedir(dir);
}

__attribute__((constructor))
static void injection_init() {
    
#if !TARGET_OS_OSX
    if (CFBundleGetMainBundle() && CFBundleGetIdentifier(CFBundleGetMainBundle())) {
        if (CFStringCompare(CFBundleGetIdentifier(CFBundleGetMainBundle()), CFSTR("com.apple.SpringBoard"), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
            dlopen(MOBILESAFETY_PATH, RTLD_NOW);
        }
    }
    
    if (!access("/var/mobile/.eksafemode", F_OK)) {
        return;
    }
#endif
    
    const char* extension = getenv("SANDBOX_EXTENSION");
    if (extension) {
        sandbox_extension_consume(extension);
    }
    tweaks_iterate();
}
