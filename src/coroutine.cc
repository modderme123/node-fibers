#include "coroutine.h"
#include "v8-version.h"
#include <assert.h>
#ifndef WINDOWS
#include <pthread.h>
#else
#include <windows.h>
#include <intrin.h>
// Stub pthreads into Windows approximations
#define pthread_t HANDLE
#define pthread_create(thread, attr, fn, arg) !((*thread)=CreateThread(NULL, 0, &(fn), arg, 0, NULL))
#define pthread_join(thread, arg) WaitForSingleObject((thread), INFINITE)
#define pthread_key_t DWORD
#define pthread_key_create(key, dtor) (*key)=TlsAlloc()
#define pthread_setspecific(key, val) TlsSetValue((key), (val))
#define pthread_getspecific(key) TlsGetValue((key))
#endif

#include <stdexcept>
#include <stack>
#include <vector>
using namespace std;

const size_t v8_tls_keys = 3;
static std::vector<void*> fls_data_pool;
static pthread_key_t coro_thread_key = 0;

static size_t stack_size = 0;
static size_t coroutines_created_ = 0;
static vector<Coroutine*> fiber_pool;
static Coroutine* delete_me = NULL;
size_t Coroutine::pool_size = 120;

static bool can_poke(void* addr) {
#ifdef WINDOWS
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQueryEx(GetCurrentProcess(), addr, &mbi, sizeof(mbi))) {
		return false;
	}
	if (!(mbi.State & MEM_COMMIT)) {
		return false;
	}
	return true;
#else
	// TODO?
	return addr > (void*)0x1000;
#endif
}

/**
 * Coroutine class definition
 */
void Coroutine::init(v8::Isolate* isolate) {
	pthread_key_create(&coro_thread_key, NULL);
	pthread_setspecific(coro_thread_key, &current());
}

Coroutine& Coroutine::current() {
	Coroutine* current = static_cast<Coroutine*>(pthread_getspecific(coro_thread_key));
	if (!current) {
		current = new Coroutine;
		pthread_setspecific(coro_thread_key, current);
	}
	return *current;
}

void Coroutine::set_stack_size(unsigned int size) {
	assert(!stack_size);
	stack_size = size;
}

size_t Coroutine::coroutines_created() {
	return coroutines_created_;
}

void Coroutine::trampoline(void* that) {
#ifdef CORO_PTHREAD
	pthread_setspecific(coro_thread_key, that);
#endif
	while (true) {
		static_cast<Coroutine*>(that)->entry(const_cast<void*>(static_cast<Coroutine*>(that)->arg));
	}
}

Coroutine::Coroutine() :
	fls_data(v8_tls_keys),
	entry(NULL),
	arg(NULL) {
	stack.sptr = NULL;
	coro_create(&context, NULL, NULL, NULL, 0);
}

Coroutine::Coroutine(entry_t& entry, void* arg) :
	fls_data(v8_tls_keys),
	entry(entry),
	arg(arg) {
}

Coroutine::~Coroutine() {
	if (stack.sptr) {
		coro_stack_free(&stack);
	}
	(void)coro_destroy(&context);
}

Coroutine* Coroutine::create_fiber(entry_t* entry, void* arg) {
	if (!fiber_pool.empty()) {
		Coroutine* fiber = fiber_pool.back();
		fiber_pool.pop_back();
		fiber->reset(entry, arg);
		return fiber;
	}
	Coroutine* coro = new Coroutine(*entry, arg);
	if (!coro_stack_alloc(&coro->stack, stack_size)) {
		delete coro;
		return NULL;
	}
	coro_create(&coro->context, trampoline, coro, coro->stack.sptr, coro->stack.ssze);
	++coroutines_created_;
	return coro;
}

void Coroutine::reset(entry_t* entry, void* arg) {
	assert(entry != NULL);
	this->entry = entry;
	this->arg = arg;
}

void Coroutine::transfer(Coroutine& next) {
	assert(this != &next);

	// next
	// fls_data[0] = pthread_getspecific(isolate_key);
	// fls_data[1] = pthread_getspecific(thread_id_key);
	// fls_data[2] = pthread_getspecific(thread_data_key);

	// pthread_setspecific(isolate_key, next.fls_data[0]);
	// pthread_setspecific(thread_id_key, next.fls_data[1]);
	// pthread_setspecific(thread_data_key, next.fls_data[2]);

	pthread_setspecific(coro_thread_key, &next);

	coro_transfer(&context, &next.context);

	pthread_setspecific(coro_thread_key, this);
}

void Coroutine::run() {
	Coroutine& current = Coroutine::current();
	assert(!delete_me);
	assert(&current != this);
	current.transfer(*this);

	if (delete_me) {
		// This means finish() was called on the coroutine and the pool was full so this coroutine needs
		// to be deleted. We can't delete from inside finish(), because that would deallocate the
		// current stack. However we CAN delete here, we just have to be very careful.
		assert(delete_me == this);
		assert(&current != this);
		delete_me = NULL;
		delete this;
	}
}

void Coroutine::finish(Coroutine& next, v8::Isolate* isolate) {
	{
		assert(&next != this);
		assert(&current() == this);
		if (fiber_pool.size() < pool_size) {
			fiber_pool.push_back(this);
		} else {
			// Clean up isolate data
			isolate->DiscardThreadSpecificMetadata();
			// Can't delete right now because we're currently on this stack!
			assert(delete_me == NULL);
			delete_me = this;
		}
	}
	this->transfer(next);
}

void* Coroutine::bottom() const {
	return stack.sptr;
}

size_t Coroutine::size() const {
	return sizeof(Coroutine) + stack_size * sizeof(void*);
}
