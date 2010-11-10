module Orocos
    module RobyPlugin
        class << self
            # The set of known process servers.
            #
            # It maps the server name to the Orocos::ProcessServer instance
            attr_reader :process_servers
        end
        @process_servers = Hash.new

        # The module in which all deployment models are defined
        module Deployments
        end

        # In oroGen, a deployment is a Unix process that holds a certain number
        # of task contexts. This Roby task represents the unix process itself.
        # Once it gets instanciated, the associated task contexts can be
        # accessed with #task(name)
        class Deployment < ::Roby::Task
            attr_accessor :robot

            def initialize(arguments = Hash.new)
	    	opts, task_arguments = Kernel.filter_options  arguments, :log => true
		task_arguments[:log] = opts[:log]
                super(task_arguments)
	    end

            class << self
                # The Orocos::Generation::StaticDeployment that represents this
                # deployment.
                attr_reader :orogen_spec

                def all_deployments; @@all_deployments end
            end
            @@all_deployments = Hash.new

            # Returns the name of this particular deployment instance
            def self.deployment_name
                orogen_spec.name
            end

            # The Orocos::Generation::StaticDeployment object describing this
            # deployment. This is a shortcut for deployment.model.orogen_spec
            def orogen_spec; self.class.orogen_spec end

            # The name of the executable, i.e. the name of the deployment as
            # given in the oroGen file
            #
            # This  is a shortcut for deployment.model.deployment_name
            def deployment_name
                orogen_spec.name
            end

            # A name => Orocos::TaskContext instance mapping of all the task
            # contexts running on this deployment
            attr_reader :task_handles

            # The underlying Orocos::Process instance
            attr_reader :orogen_deployment

            ##
            # :method: ready_event
            #
            # Event emitted when the deployment is up and running
            event :ready

            ##
            # :method: signaled_event
            #
            # Event emitted whenever the deployment finishes because of a UNIX
            # signal. The event context is the Process::Status instance that
            # describes the termination
            #
            # It is forwarded to failed_event
            event :signaled
            forward :signaled => :failed

            # An array of Orocos::Generation::TaskDeployment instances that
            # represent the tasks available in this deployment. Associated plan
            # objects can be instanciated with #task
            def self.tasks
                orogen_spec.task_activities
            end

            def instanciate_all_tasks
                orogen_spec.task_activities.map do |act|
                    task(act.name)
                end
            end

            # Returns an task instance that represents the given task in this
            # deployment.
            def task(name)
                activity = orogen_spec.task_activities.find { |act| name == act.name }
                if !activity
                    raise ArgumentError, "no task called #{name} in #{self.class.deployment_name}"
                end

                klass = Roby.app.orocos_tasks[activity.context.name]
                plan.add(task = klass.new)
                task.robot = robot
                task.executed_by self
                task.orogen_spec = activity
                if ready?
                    task.orogen_task = task_handles[name]
                    task.orogen_task.process = orogen_deployment
                end
                task
            end

            ##
            # method: start!
            #
            # Starts the process and emits the start event immediately. The
            # :ready event will be emitted when the deployment is up and
            # running.
            event :start do |context|
                host = self.arguments['on'] ||= 'localhost'
                RobyPlugin.info { "starting deployment #{model.deployment_name} on #{host}" }

                process_server, log_dir = Orocos::RobyPlugin.process_servers[host]
                @orogen_deployment = process_server.start(model.deployment_name, :working_directory => log_dir, :output => "%m-%p.txt")
                Deployment.all_deployments[@orogen_deployment] = self
                emit :start
            end

            def log_dir
                host = self.arguments['on'] ||= 'localhost'
                process_server, log_dir = Orocos::RobyPlugin.process_servers[host]
                log_dir
            end

            # The name of the machine this deployment is running on, i.e. the
            # name given to the :on argument.
            def machine
                arguments[:on] || 'localhost'
            end

            # Called when the process is finished.
            #
            # +result+ is the Process::Status object describing how this process
            # finished.
            def dead!(result)
                if !result
                    emit :failed
                elsif result.success?
                    emit :success
                elsif result.signaled?
                    emit :signaled, result
                else
                    emit :failed, result
                end

                Deployment.all_deployments.delete(orogen_deployment)
                orogen_spec.task_activities.each do |act|
                    TaskContext.configured.delete(act.name)
                end
                each_parent_object(Roby::TaskStructure::ExecutionAgent) do |task|
                    task.orogen_task = nil
                end
            end

            # Removes any connection that points to tasks that were in this
            # process, and that are therefore dead because of the process
            # termination
            def cleanup_dead_connections
                return if !task_handles

                # task_handles is only initialized when ready is reached ...
                # so can be nil here
                all_tasks = task_handles.values.to_value_set
                all_tasks.each do |task|
                    task.each_parent_vertex(ActualDataFlow) do |parent_task|
                        if parent_task.process
                            next if !parent_task.process.running?
                            roby_task = Deployment.all_deployments[parent_task.process]
                            next if roby_task.finishing? || roby_task.finished?
                        end

                        mappings = parent_task[task, ActualDataFlow]
                        mappings.each do |(source_port, sink_port), policy|
                            if policy[:pull] # we have to disconnect explicitely
                                begin
                                    parent_task.port(source_port).disconnect_from(task.port(sink_port, false))
                                rescue Exception => e
                                    Orocos::RobyPlugin.warn "error while disconnecting #{parent_task}:#{source_port} from #{task}:#{sink_port} after #{task} died (#{e.message}). Assuming that both tasks are already dead."
                                end
                            end
                        end
                    end
                    task.each_child_vertex(ActualDataFlow) do |child_task|
                        if child_task.process
                            next if !child_task.process.running?
                            roby_task = Deployment.all_deployments[child_task.process]
                            next if roby_task.finishing? || roby_task.finished?
                        end

                        mappings = task[child_task, ActualDataFlow]
                        mappings.each do |(source_port, sink_port), policy|
                            begin
                                child_task.port(sink_port).disconnect_all
                            rescue Exception => e
                                Orocos::RobyPlugin.warn "error while disconnecting #{task}:#{source_port} from #{child_task}:#{sink_port} after #{task} died (#{e.message}). Assuming that both tasks are already dead."
                            end
                        end
                    end

                    ActualDataFlow.remove(task)
                    RequiredDataFlow.remove(task)
                end
            end

            poll do
                begin
                    next if ready?

                    if orogen_deployment.wait_running(0)
                        @task_handles = Hash.new
                        orogen_spec.task_activities.each do |activity|
                            task_handles[activity.name] = 
                                ::Orocos::TaskContext.get(activity.name)
                        end

                        if !arguments[:log] || Roby::State.orocos.deployment_excluded_from_log?(self)
                            Robot.info "not automatically logging any port in deployment #{name}"
                        else
                            Orocos::Process.log_all_ports(orogen_deployment,
                                        :log_dir => log_dir,
                                        :remote => (machine != 'localhost')) do |port|

                                result = !Roby::State.orocos.port_excluded_from_log?(self, Roby.app.orocos_tasks[port.task.model.name], port)
                                if !result
                                    Robot.info "not logging #{port.task.name}:#{port.name}"
                                end
                                result
                            end
                        end

                        each_parent_object(Roby::TaskStructure::ExecutionAgent) do |task|
                            task.orogen_task = task_handles[task.orocos_name]
                            task.orogen_task.process = orogen_deployment
                        end

                        emit :ready
                    end
                rescue Exception => e
                    STDERR.puts e.message
                    raise
                end
            end

	    def ready_to_die!
	    	@ready_to_die = true
	    end

	    attr_predicate :ready_to_die?

            ##
            # method: stop!
            #
            # Stops all tasks that are running on top of this deployment, and
            # kill the deployment
            event :stop do |context|
		ready_to_die!
                orogen_deployment.kill(false)
            end

            # Creates a subclass of Deployment that represents the deployment
            # specified by +deployment_spec+.
            #
            # +deployment_spec+ is an instance of Orogen::Generation::Deployment
            def self.define_from_orogen(deployment_spec)
                klass = Class.new(Deployment)
                klass.instance_variable_set :@name, "Orocos::RobyPlugin::Deployments::#{deployment_spec.name.camelcase(true)}"
                klass.instance_variable_set :@orogen_spec, deployment_spec
                Orocos::RobyPlugin::Deployments.const_set(deployment_spec.name.camelcase(true), klass)
                klass
            end
        end
    end
end

