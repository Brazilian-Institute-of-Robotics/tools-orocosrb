#include "ControlTaskC.h"
#include "DataFlowC.h"
#include <ruby.h>

#include "corba.hh"
#include "misc.hh"
#include <memory>

using namespace std;
static VALUE mOrocos;
static VALUE cTaskContext;
static VALUE cInputPort;
static VALUE cOutputPort;
static VALUE cPort;
static VALUE cAttribute;
VALUE eNotFound;

using namespace RTT::Corba;


struct RTaskContext
{
    RTT::Corba::ControlTask_var        task;
    RTT::Corba::DataFlowInterface_var  ports;
    RTT::Corba::AttributeInterface_var attributes;
    RTT::Corba::MethodInterface_var    methods;
    RTT::Corba::CommandInterface_var   commands;
};

struct RInputPort { };
struct ROutputPort { };

struct RAttribute
{
    RTT::Corba::Expression_var expr;
};

/* call-seq:
 *  Orocos.components => [name1, name2, name3, ...]
 *
 * Returns the names of the task contexts registered with Corba
 */
static VALUE orocos_task_names(VALUE mod)
{
    VALUE result = rb_ary_new();

    list<string> names = CorbaAccess::knownTasks();
    for (list<string>::const_iterator it = names.begin(); it != names.end(); ++it)
        rb_ary_push(result, rb_str_new2(it->c_str()));

    return result;
}

/* call-seq:
 *  TaskContext.get(name) => task
 *
 * Returns the TaskContext instance representing the remote task context
 * with the given name. Raises Orocos::NotFound if the task name does
 * not exist.
 */
static VALUE task_context_get(VALUE klass, VALUE name)
{
    try {
        std::auto_ptr<RTaskContext> new_context( new RTaskContext );
        new_context->task       = CorbaAccess::findByName(StringValuePtr(name));
        new_context->ports      = new_context->task->ports();
        new_context->attributes = new_context->task->attributes();
        new_context->methods    = new_context->task->methods();
        new_context->commands   = new_context->task->commands();

        VALUE obj = simple_wrap(cTaskContext, new_context.release());
        rb_iv_set(obj, "@name", rb_str_dup(name));
        return obj;
    }
    catch(...) {
        rb_raise(eNotFound, "task context '%s' not found", StringValuePtr(name));
    }
}

static VALUE task_context_equal_p(VALUE self, VALUE other)
{
    if (!rb_obj_is_kind_of(other, cTaskContext))
        return Qfalse;

    RTaskContext& self_  = get_wrapped<RTaskContext>(self);
    RTaskContext& other_ = get_wrapped<RTaskContext>(other);
    return self_.task->_is_equivalent(other_.task) ? Qtrue : Qfalse;
}

/* call-seq:
 *   task.port(name) => port
 *
 * Returns the Orocos::DataPort or Orocos::BufferPort object representing the
 * remote port +name+. Raises RuntimeError if the port does not exist.
 */
static VALUE task_context_port(VALUE self, VALUE name)
{
    RTaskContext& context = get_wrapped<RTaskContext>(self);
    RTT::Corba::PortType port_type;
    try {
        port_type      = context.ports->getPortType(StringValuePtr(name));
    }
    catch(RTT::Corba::NoSuchPortException)
    { 
        VALUE task_name = rb_iv_get(self, "@name");
        rb_raise(eNotFound, "task %s does not have a '%s' port",
                StringValuePtr(task_name),
                StringValuePtr(name));
    }

    VALUE obj = Qnil;
    if (port_type == RTT::Corba::Input)
    {
        auto_ptr<RInputPort> rport( new RInputPort );
        obj = simple_wrap(cInputPort, rport.release());
    }
    else if (port_type == RTT::Corba::Output)
    {
        auto_ptr<ROutputPort> rport( new ROutputPort );
        obj = simple_wrap(cOutputPort, rport.release());
    }

    rb_iv_set(obj, "@name", rb_str_dup(name));
    rb_iv_set(obj, "@task", self);
    VALUE type_name = rb_str_new2(context.ports->getDataType( StringValuePtr(name) ));
    rb_iv_set(obj, "@typename", type_name);
    return obj;
}

/* call-seq:
 *  task.each_port { |p| ... } => task
 *
 * Enumerates the ports available on this task. This yields instances of either
 * Orocos::BufferPort or Orocos::DataPort
 */
static VALUE task_context_each_port(VALUE self)
{
    RTaskContext& context = get_wrapped<RTaskContext>(self);
    RTT::Corba::DataFlowInterface::PortNames_var ports = context.ports->getPorts();

    for (int i = 0; i < ports->length(); ++i)
        rb_yield(task_context_port(self, rb_str_new2(ports[i])));

    return self;
}

/* call-seq:
 *  task.attribute(name) => attribute
 *
 * Returns the Attribute object which represents the remote task's
 * Attribute or Property of the given name
 */
static VALUE task_context_attribute(VALUE self, VALUE name)
{
    RTaskContext& context = get_wrapped<RTaskContext>(self);
    std::auto_ptr<RAttribute> rattr(new RAttribute);
    rattr->expr = context.attributes->getProperty( StringValuePtr(name) );
    if (CORBA::is_nil(rattr->expr))
        rattr->expr = context.attributes->getAttribute( StringValuePtr(name) );
    if (CORBA::is_nil(rattr->expr))
        rb_raise(eNotFound, "no attribute or property named '%s'", StringValuePtr(name));

    VALUE type_name = rb_str_new2(rattr->expr->getTypeName());
    VALUE obj = simple_wrap(cAttribute, rattr.release());
    rb_iv_set(obj, "@name", rb_str_dup(name));
    rb_iv_set(obj, "@typename", type_name);
    return obj;
}

/* call-seq:
 *  task.each_attribute { |a| ... } => task
 *
 * Enumerates the attributes and properties that are available on
 * this task, as instances of Orocos::Attribute
 */
static VALUE task_context_each_attribute(VALUE self)
{
    RTaskContext& context = get_wrapped<RTaskContext>(self);

    {
        RTT::Corba::AttributeInterface::AttributeNames_var
            attributes = context.attributes->getAttributeList();
        for (int i = 0; i < attributes->length(); ++i)
            rb_yield(task_context_attribute(self, rb_str_new2(attributes[i])));
    }

    {
        RTT::Corba::AttributeInterface::PropertyNames_var
            properties = context.attributes->getPropertyList();
        for (int i = 0; i < properties->length(); ++i)
            rb_yield(task_context_attribute(self, rb_str_new2(properties[i].name)));
    }
}

/* call-seq:
 *  task.state => value
 *
 * Returns the state of the task, as an integer value. The possible values are
 * represented by the various +STATE_+ constants:
 * 
 *   STATE_PRE_OPERATIONAL
 *   STATE_STOPPED
 *   STATE_ACTIVE
 *   STATE_RUNNING
 *   STATE_RUNTIME_WARNING
 *   STATE_RUNTIME_ERROR
 *   STATE_FATAL_ERROR
 *
 * See Orocos own documentation for their meaning
 */
static VALUE task_context_state(VALUE obj)
{
    RTaskContext& context = get_wrapped<RTaskContext>(obj);
    return INT2FIX(context.task->getTaskState());
}

/* call-seq:
 *  port.connect(remote_port) => nil
 *
 * Connects the given output port to the specified remote port
 */
//static VALUE port_do_connect(VALUE self, VALUE other_port)
//{
//    VALUE src_task_r = rb_iv_get(self, "@task");
//    VALUE src_name   = rb_iv_get(self, "@name");
//    RTaskContext& src_task = get_wrapped<RTaskContext>(src_task_r);
//    VALUE dst_task_r = rb_iv_get(remote_port, "@task");
//    VALUE dst_name   = rb_iv_get(remote_port, "@name");
//    RTaskContext& dst_task = get_wrapped<RTaskContext>(dst_task_r);
//
//    if (!src_task.ports->connectPorts(StringValuePtr(src_name), dst_task.ports, StringValuePtr(dst_name)))
//    {
//        VALUE src_task_name = rb_iv_get(src_task_r, "@name");
//        VALUE dst_task_name = rb_iv_get(dst_task_r, "@name");
//        rb_raise(rb_eArgError, "cannot connect %s.%s to %s.%s",
//                StringValuePtr(src_task_name),
//                StringValuePtr(src_name),
//                StringValuePtr(dst_task_name),
//                StringValuePtr(dst_name));
//    }
//    return Qnil;
//}

/* call-seq:
 *   port.disconnect => nil
 *
 * Remove all connections that go to or come from this port
 */
//static VALUE port_disconnect(VALUE self)
//{
//
//    VALUE task_r = rb_iv_get(self, "@task");
//    VALUE name   = rb_iv_get(self, "@name");
//    RTaskContext& task = get_wrapped<RTaskContext>(task_r);
//    task.ports->disconnect(StringValuePtr(name));
//    return Qnil;
//}

/* call-seq:
 *  port.connected? => true or false
 *
 * Tests if this port is already part of a connection or not
 */
static VALUE port_connected_p(VALUE self)
{

    VALUE task_r = rb_iv_get(self, "@task");
    VALUE name   = rb_iv_get(self, "@name");
    RTaskContext& task = get_wrapped<RTaskContext>(task_r);
    return task.ports->isConnected(StringValuePtr(name)) ? Qtrue : Qfalse;
}

/* Document-class: Orocos::TaskContext
 *
 * TaskContext in Orocos are the representation of the component: access to its
 * inputs and outputs (#each_port) and to its execution state (#state).
 *
 */
/* Document-class: Orocos::Port
 *
 * Ports are in Orocos the representation of the task's dynamic input and
 * output. Port instances are never used. The task's ports are either represented
 * by instances of Orocos::DataPort or Orocos::BufferPort.
 */
/* Document-class: Orocos::DataPort
 *
 * Ports are in Orocos the representation of the task's dynamic input and
 * output. In data ports, the data you read from the port is the last sample
 * ever written to it.
 *
 * See also Orocos::Port and Orocos::BufferPort
 */
/* Document-class: Orocos::BufferPort
 *
 * Ports are in Orocos the representation of the task's dynamic input and
 * output. In buffered ports, the data is kept as long as it is not read, meaning
 * that the target of the data link should be able to get all the samples ever
 * written on the wire.
 *
 * In Orocos, this is limited to a fixed number of samples (the size of the
 * buffer), so if the buffer is full, samples are still lost.
 *
 * See also Orocos::Port and Orocos::DataPort
 */
/* Document-class: Orocos::NotFound
 *
 * This exception is raised every time an Orocos object is required by name,
 * but the object does not exist.
 *
 * See for instance Orocos::TaskContext.get or Orocos::TaskContext#port
 */
/* Document-class: Orocos::Attribute
 *
 * Attributes and properties are in Orocos ways to parametrize the task contexts.
 * Instances of Orocos::Attribute actually represent both at the same time.
 */
/* Document-module: Orocos
 */
extern "C" void Init_rorocos_ext()
{
    mOrocos = rb_define_module("Orocos");
    mCORBA  = rb_define_module_under(mOrocos, "CORBA");

    cTaskContext = rb_define_class_under(mOrocos, "TaskContext", rb_cObject);
    rb_const_set(cTaskContext, rb_intern("STATE_PRE_OPERATIONAL"),      INT2FIX(RTT::Corba::PreOperational));
    rb_const_set(cTaskContext, rb_intern("STATE_FATAL_ERROR"),          INT2FIX(RTT::Corba::FatalError));
    rb_const_set(cTaskContext, rb_intern("STATE_STOPPED"),              INT2FIX(RTT::Corba::Stopped));
    rb_const_set(cTaskContext, rb_intern("STATE_ACTIVE"),               INT2FIX(RTT::Corba::Active));
    rb_const_set(cTaskContext, rb_intern("STATE_RUNNING"),              INT2FIX(RTT::Corba::Running));
    rb_const_set(cTaskContext, rb_intern("STATE_RUNTIME_WARNING"),      INT2FIX(RTT::Corba::RunTimeWarning));
    rb_const_set(cTaskContext, rb_intern("STATE_RUNTIME_ERROR"),        INT2FIX(RTT::Corba::RunTimeError));
    
    cPort        = rb_define_class_under(mOrocos, "Port", rb_cObject);
    cOutputPort  = rb_define_class_under(mOrocos, "OutputPort", cPort);
    cInputPort   = rb_define_class_under(mOrocos, "InputPort", cPort);
    cAttribute   = rb_define_class_under(mOrocos, "Attribute", rb_cObject);
    eNotFound    = rb_define_class_under(mOrocos, "NotFound", rb_eRuntimeError);

    rb_define_singleton_method(mOrocos, "task_names", RUBY_METHOD_FUNC(orocos_task_names), 0);
    rb_define_singleton_method(cTaskContext, "get", RUBY_METHOD_FUNC(task_context_get), 1);
    rb_define_method(cTaskContext, "==", RUBY_METHOD_FUNC(task_context_equal_p), 1);
    rb_define_method(cTaskContext, "state", RUBY_METHOD_FUNC(task_context_state), 0);
    rb_define_method(cTaskContext, "port", RUBY_METHOD_FUNC(task_context_port), 1);
    rb_define_method(cTaskContext, "each_port", RUBY_METHOD_FUNC(task_context_each_port), 0);
    rb_define_method(cTaskContext, "attribute", RUBY_METHOD_FUNC(task_context_attribute), 1);
    rb_define_method(cTaskContext, "each_attribute", RUBY_METHOD_FUNC(task_context_each_attribute), 0);

    Orocos_CORBA_init();
//    rb_define_method(cPort, "do_connect", RUBY_METHOD_FUNC(port_do_connect), 1);
//    rb_define_method(cPort, "disconnect", RUBY_METHOD_FUNC(port_disconnect), 0);
//    rb_define_method(cPort, "connected?", RUBY_METHOD_FUNC(port_connected_p), 0);
}

