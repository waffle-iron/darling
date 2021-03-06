/*
This file is part of Darling.

Copyright (C) 2013 Lubos Dolezel

Darling is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Darling is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Darling.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "TLS.h"
#include <cstdlib>
#include <map>
#include <cassert>
#include <cstring>
#include <objc/blocks_runtime.h>
#include <vector>
#include "log.h"
#include "trace.h"
#include "mutex.h"
#include "dyld_priv.h"

extern "C" {
int __darwin_pthread_key_create(long *key, void (*destructor)(void*));
int __darwin_pthread_key_delete(long key);
int __darwin_pthread_setspecific(long key, const void *value);
}

static void TLSRunDestructors(void* p);

static std::map<void*, Darling::ImageData*> m_images;
static std::map<pthread_key_t, Darling::ImageData*> m_imagesKey;
static Darling::Mutex m_imagesMutex;
struct tlv_listener
{
	enum dyld_tlv_states state;
	dyld_tlv_state_change_handler handler;
};

static std::vector<tlv_listener> g_tlvListeners;
static void notifyTLSListeners(enum dyld_tlv_states state, void* mem, size_t size);

// Used for a thread-local list of destructor-object pairs (DestructorLinkedListElement)
static long m_keyDestructors;
static std::map<void*,size_t> g_allocatedTLS;

__attribute__((weak)) void *_Block_copy(void *);

static void TLSSetupDestructors()
{
	__darwin_pthread_key_create(&m_keyDestructors, TLSRunDestructors);
}

static void TLSRunDestructors(void* p)
{
	TRACE1(p);
	
	Darling::DestructorLinkedListElement* dlist = static_cast<Darling::DestructorLinkedListElement*>(p);
	
	dlist = dlist->first;
	
	do
	{
		Darling::DestructorLinkedListElement* dthis = dlist;
		
		dlist->destructor(dlist->object);
		dlist = dlist->next;
		
		delete dthis;
	}
	while (dlist != nullptr);
}

void Darling::TLSSetup(void* imageKey, std::vector<tlv_descriptor*>& descriptors, std::vector<void*>& initializers, void* start, uintptr_t length)
{
	Darling::MutexLock _l(&m_imagesMutex);
	long key;
	ImageData* id;
	pthread_once_t once;

	pthread_once(&once, TLSSetupDestructors);

	auto it = m_images.find(imageKey);
	assert (it == m_images.end());

	__darwin_pthread_key_create(&key, (void (*)(void *)) Darling::TLSTeardownThread);
	
	m_images[imageKey] = id = new ImageData{ key, start, length, initializers };
	m_imagesKey[key] = id;

	for (tlv_descriptor* d : descriptors)
	{
		d->thunk = Darling::TLSGetAddr;
		d->key = key;
	}
}

void Darling::TLSTeardown(void* imageKey)
{
	TRACE1(imageKey);
	
	Darling::MutexLock _l(&m_imagesMutex);
	auto it = m_images.find(imageKey);
	long key;

	if (it == m_images.end())
		return;

	key = it->second->key;
	
	// TODO: What to do with live objects with C++11 destructors? Does Apple's dyld handle this?

	m_imagesKey.erase(key);
	__darwin_pthread_key_delete(key);
	
	delete it->second;
	m_images.erase(it);
}

void Darling::TLSTeardownThread(Darling::ThreadData data)
{
	Darling::MutexLock _l(&m_imagesMutex);
	TRACE1((void*) data);

	auto it = g_allocatedTLS.find(data);
	if (it != g_allocatedTLS.end())
	{
		notifyTLSListeners(dyld_tlv_state_deallocated, it->first, it->second);
		g_allocatedTLS.erase(it);
	}

	delete [] data;
}

void* Darling::TLSAllocate(pthread_key_t key)
{
	TRACE1(key);
	
	Darling::MutexLock _l(&m_imagesMutex);
	
	Darling::ImageData* id = m_imagesKey[key];
	char* data;
	
	// Allocate a new block for TLS variables
	data = new char[id->length];
	
	// Copy the initial variable values into the new block
	memcpy(data, id->start, id->length);

	// Run initializers (constructors etc.)
	for (auto it = id->initializers.rbegin(); it != id->initializers.rend(); it++)
	{
		typedef void (*Initializer)();
		Initializer in = Initializer(*it);
		in();
	}

	// Delete the data upon thread exit
	__darwin_pthread_setspecific(key, data);
	g_allocatedTLS[data] = id->length;
	notifyTLSListeners(dyld_tlv_state_allocated, data, id->length);
	
	LOG << "TLS data allocated at " << (void*)data << std::endl;

	return data;
}

void Darling::TLSAtExit(void (*destructor)(void*), void* object)
{
	TRACE2(destructor, object);
	
	Darling::DestructorLinkedListElement* last = static_cast<Darling::DestructorLinkedListElement*>(pthread_getspecific(m_keyDestructors));
	Darling::DestructorLinkedListElement* d = new Darling::DestructorLinkedListElement;
	
	d->destructor = destructor;
	d->object = object;
	d->next = nullptr;
	
	if (last != nullptr)
	{
		d->first = last->first;
		last->next = d;
	}
	else
	{
		d->first = d;
	}
	
	__darwin_pthread_setspecific(m_keyDestructors, d);
}

void* Darling::TLSBootstrap(tlv_descriptor* desc)
{
	LOG << "Darling::TLSBootstrap() called, this should never happen\n";
	abort();
}

void dyld_register_tlv_state_change_handler(enum dyld_tlv_states state, dyld_tlv_state_change_handler handler)
{
	g_tlvListeners.push_back(tlv_listener { state, Block_copy(handler) });
}

void dyld_enumerate_tlv_storage(dyld_tlv_state_change_handler handler)
{
	Darling::MutexLock _l(&m_imagesMutex);
	for (auto it = g_allocatedTLS.begin(); it != g_allocatedTLS.end(); it++)
	{
		const dyld_tlv_info info = { sizeof(dyld_tlv_info), it->first, it->second };
		handler(dyld_tlv_state_allocated, &info);
	}
}

static void notifyTLSListeners(enum dyld_tlv_states state, void* mem, size_t size)
{
	const dyld_tlv_info info = { sizeof(dyld_tlv_info), mem, size };
	for (auto it = g_tlvListeners.begin(); it != g_tlvListeners.end(); it++)
	{
		if (it->state == state)
			it->handler(state, &info);
	}
}

